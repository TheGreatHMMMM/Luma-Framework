#define GAME_DYING_LIGHT 1

#define CHECK_GRAPHICS_API_COMPATIBILITY 1

// DRM (Steam) may interfere with auto-debugger attachment
#define DISABLE_AUTO_DEBUGGER 1

#define DISABLE_FOCUS_LOSS_SUPPRESSION 1

#define ENABLE_NGX 1

#include "../../../Shaders/Dying Light/Includes/GameCBuffers.hlsl"
#include "..\..\Core\core.hpp"

namespace
{
   std::set<reshade::api::format> toggleable_texture_upgrade_formats;

   ShaderHashesList shader_hashes_TAA;             // 0xA67ABF78 – SMAA T2X pass
   ShaderHashesList shader_hashes_MV;              // 0x923B088C – motion vector pass; t0 is the scene depth buffer

   bool has_drawn_taa = false;

   com_ptr<ID3D11Resource> depth;

   float2 frame_jitters      = float2{0.f, 0.f};
   float2 prev_frame_jitters = float2{0.f, 0.f};

   struct Matrix3x4 { float m[3][4]; };
   struct Matrix4x4 { float m[4][4]; };

   struct CameraStruct
   {
      char pad_0000[0x20];      // 0x0000
      Matrix3x4 View;           // 0x0020
      Matrix3x4 Cam;            // 0x0050
      Matrix4x4 weirdProj;      // 0x0080
      Matrix4x4 VP;             // 0x00C0
      Matrix4x4 noIdea;         // 0x0100
      char pad_0140[0x30];      // 0x0140
      float zNear;              // 0x0170
      float zFar;               // 0x0174
      char pad_0178[0x188];     // 0x0178
      Matrix4x4 RealProjWithJit;// 0x0300
      char pad_0340[0xED0];     // 0x0340
   };

   static_assert(offsetof(CameraStruct, zNear)         == 0x0170);
   static_assert(offsetof(CameraStruct, RealProjWithJit) == 0x0300);

   uintptr_t camStructBase   = 0;
   uint8_t*  cam_hook_memory = nullptr;

   volatile float g_pixel_jitter_x = 0.f;
   volatile float g_pixel_jitter_y = 0.f;
   uint8_t* jitter_hook_memory = nullptr;

   static constexpr int kHaltonCount = 8;
   alignas(8) float g_halton_table[kHaltonCount * 2] = {};
   uint32_t         g_frame_index   = 0;

   void InitHaltonTable()
   {
      for (int i = 0; i < kHaltonCount; ++i)
      {
         g_halton_table[i * 2 + 0] = SR::HaltonSequence(i + 1, 2);
         g_halton_table[i * 2 + 1] = SR::HaltonSequence(i + 1, 3);
      }
   }

   CameraStruct* GetCamera()
   {
      if (!camStructBase)
         return nullptr;
      return reinterpret_cast<CameraStruct*>(camStructBase + 0x7CE0);
   }

   // hook from z1rp
   bool InitCameraHook()
   {
      HMODULE engineModule = GetModuleHandleA("engine_x64_rwdi.dll");
      if (!engineModule)
         return false;

      constexpr size_t stolenLen = 12;
      const uintptr_t baseAddr  = (uintptr_t)engineModule;
      const uintptr_t patchAddr = baseAddr + 0x75F3CB;
      const uintptr_t retAddr   = patchAddr + stolenLen;

      cam_hook_memory = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
      if (!cam_hook_memory)
         return false;

      uint8_t capture[] = {
         0x48, 0xB8,                                      // mov rax, imm64
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // <-- &camStructAddr
         0x48, 0x89, 0x30                                 // mov [rax], rsi
      };
      uintptr_t camStructBaseAddr = (uintptr_t)&camStructBase;
      memcpy(&capture[2], &camStructBaseAddr, sizeof(uintptr_t));

      uint8_t stolenBytes[stolenLen];
      memcpy(stolenBytes, (void*)patchAddr, stolenLen);

      uint8_t jmpBack[] = {
         0x48, 0xB8,                                      // mov rax, imm64
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 8-byte address
         0xFF, 0xE0                                       // jmp rax
      };
      memcpy(&jmpBack[2], &retAddr, sizeof(uintptr_t));

      size_t off = 0;
      memcpy(cam_hook_memory + off, capture,     sizeof(capture));     off += sizeof(capture);
      memcpy(cam_hook_memory + off, stolenBytes, stolenLen);            off += stolenLen;
      memcpy(cam_hook_memory + off, jmpBack,     sizeof(jmpBack));

      FlushInstructionCache(GetCurrentProcess(), cam_hook_memory, 128);

      DWORD oldProtect;
      if (!VirtualProtect((void*)patchAddr, stolenLen, PAGE_EXECUTE_READWRITE, &oldProtect))
      {
         VirtualFree(cam_hook_memory, 0, MEM_RELEASE);
         cam_hook_memory = nullptr;
         return false;
      }

      uint8_t jmpToShell[] = {
         0x48, 0xB8,                                      // mov rax, imm64
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // <-- &camStructAddr
         0xFF, 0xE0                                       // jmp rax
      };
      const uintptr_t shellAddr = (uintptr_t)cam_hook_memory;
      memcpy(&jmpToShell[2], &shellAddr, sizeof(uintptr_t));

      memset((void*)patchAddr, 0x90, stolenLen);
      memcpy((void*)patchAddr, jmpToShell, sizeof(jmpToShell));

      VirtualProtect((void*)patchAddr, stolenLen, oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), (void*)patchAddr, stolenLen);

      return true;
   }

   // Mid-function hook at engine+0x75E0AF that REPLACES the SMAA T2X 2-sample
   // jitter pattern with a Halton(2,3) sequence. The function reads:
   //
   //   lea   rax, [rip + jitter_table]   ; engine+0xA14C90 = {+.25,-.25,-.25,+.25}
   //   movss xmm6, [rax + rcx*8]         ; rcx = frame & 1; xmm6 = pixel jitter X
   //   movss xmm7, [rax + rcx*8 + 4]     ;                  xmm7 = pixel jitter Y
   //   mov   rax, [rbx + 0x1010]
   //
   // We hook the 18 bytes covering all three. Instead of replaying the engine's
   // table read, we synthesize xmm6/xmm7 from g_halton_table[g_frame_index] and
   // mirror them to g_pixel_jitter_x/y. The engine then jitters its projection
   // matrix with Halton, and DLSS receives the matching pixel jitter -- both
   // sides stay consistent.
   bool InitJitterHook()
   {
      HMODULE engineModule = GetModuleHandleA("engine_x64_rwdi.dll");
      if (!engineModule)
         return false;

      InitHaltonTable();

      constexpr size_t stolenLen = 18;  // 0x75E0AF .. 0x75E0C1
      const uintptr_t baseAddr  = (uintptr_t)engineModule;
      const uintptr_t patchAddr = baseAddr + 0x75E0AF;
      const uintptr_t retAddr   = patchAddr + stolenLen;

      static const uint8_t kExpected[stolenLen] = {
         0xF3, 0x0F, 0x10, 0x34, 0xC8,                   // movss xmm6, [rax + rcx*8]
         0xF3, 0x0F, 0x10, 0x7C, 0xC8, 0x04,             // movss xmm7, [rax + rcx*8 + 4]
         0x48, 0x8B, 0x83, 0x10, 0x10, 0x00, 0x00,       // mov rax, [rbx + 0x1010]
      };
      if (memcmp((void*)patchAddr, kExpected, stolenLen) != 0)
         return false;

      jitter_hook_memory = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
      if (!jitter_hook_memory)
         return false;

      // Trampoline shellcode. Layout (rax/rcx are saved/restored):
      //   push rax ; push rcx
      //   mov   rax, &g_frame_index
      //   mov   ecx, [rax]                ; idx
      //   inc   dword ptr [rax]           ; advance for next frame
      //   and   ecx, kHaltonCount-1       ; mask -> table index
      //   mov   rax, &g_halton_table
      //   movss xmm6, [rax + rcx*8]       ; Halton X (pixels)
      //   movss xmm7, [rax + rcx*8 + 4]   ; Halton Y (pixels)
      //   mov   rax, &g_pixel_jitter_x ; movss [rax], xmm6
      //   mov   rax, &g_pixel_jitter_y ; movss [rax], xmm7
      //   pop   rcx ; pop rax
      //   mov   rax, [rbx + 0x1010]       ; replay last 7 stolen bytes
      //   jmp   qword ptr [rip+0] ; abs64 retAddr
      static_assert((kHaltonCount & (kHaltonCount - 1)) == 0, "kHaltonCount must be power of two");
      static_assert(kHaltonCount <= 256, "AND immediate is 8-bit");

      uint8_t shell[] = {
         0x50,                                                  // push rax
         0x51,                                                  // push rcx
         0x48, 0xB8, 0,0,0,0,0,0,0,0,                           //  2: mov rax, &g_frame_index
         0x8B, 0x08,                                            // 12: mov ecx, [rax]
         0xFF, 0x00,                                            // 14: inc dword ptr [rax]
         0x83, 0xE1, (uint8_t)(kHaltonCount - 1),               // 16: and ecx, mask
         0x48, 0xB8, 0,0,0,0,0,0,0,0,                           // 19: mov rax, &g_halton_table
         0xF3, 0x0F, 0x10, 0x34, 0xC8,                          // 29: movss xmm6, [rax + rcx*8]
         0xF3, 0x0F, 0x10, 0x7C, 0xC8, 0x04,                    // 34: movss xmm7, [rax + rcx*8 + 4]
         0x48, 0xB8, 0,0,0,0,0,0,0,0,                           // 40: mov rax, &g_pixel_jitter_x
         0xF3, 0x0F, 0x11, 0x30,                                // 50: movss [rax], xmm6
         0x48, 0xB8, 0,0,0,0,0,0,0,0,                           // 54: mov rax, &g_pixel_jitter_y
         0xF3, 0x0F, 0x11, 0x38,                                // 64: movss [rax], xmm7
         0x59,                                                  // 68: pop rcx
         0x58,                                                  // 69: pop rax
         0x48, 0x8B, 0x83, 0x10, 0x10, 0x00, 0x00,              // 70: mov rax, [rbx + 0x1010]
         0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,                    // 77: jmp qword ptr [rip+0]
         0,0,0,0,0,0,0,0,                                       // 83: abs64 retAddr
      };
      const uintptr_t pFrame  = (uintptr_t)&g_frame_index;
      const uintptr_t pHalton = (uintptr_t)&g_halton_table[0];
      const uintptr_t pJX     = (uintptr_t)&g_pixel_jitter_x;
      const uintptr_t pJY     = (uintptr_t)&g_pixel_jitter_y;
      memcpy(&shell[ 4], &pFrame,  sizeof(pFrame));
      memcpy(&shell[21], &pHalton, sizeof(pHalton));
      memcpy(&shell[42], &pJX,     sizeof(pJX));
      memcpy(&shell[56], &pJY,     sizeof(pJY));
      memcpy(&shell[83], &retAddr, sizeof(retAddr));

      memcpy(jitter_hook_memory, shell, sizeof(shell));
      FlushInstructionCache(GetCurrentProcess(), jitter_hook_memory, 128);

      DWORD oldProtect;
      if (!VirtualProtect((void*)patchAddr, stolenLen, PAGE_EXECUTE_READWRITE, &oldProtect))
      {
         VirtualFree(jitter_hook_memory, 0, MEM_RELEASE);
         jitter_hook_memory = nullptr;
         return false;
      }

      // Patch site: 14-byte JMP [RIP+0] + abs64 + 4 NOPs (stolenLen=18).
      // Critical: this jump touches NO registers.
      uint8_t patchBytes[stolenLen];
      memset(patchBytes, 0x90, stolenLen);
      patchBytes[0] = 0xFF;
      patchBytes[1] = 0x25;
      patchBytes[2] = 0x00;
      patchBytes[3] = 0x00;
      patchBytes[4] = 0x00;
      patchBytes[5] = 0x00;
      const uintptr_t shellAddr = (uintptr_t)jitter_hook_memory;
      memcpy(&patchBytes[6], &shellAddr, sizeof(shellAddr));
      memcpy((void*)patchAddr, patchBytes, stolenLen);

      VirtualProtect((void*)patchAddr, stolenLen, oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), (void*)patchAddr, stolenLen);

      return true;
   }
} // namespace

struct DyingLightDeviceData final : public GameDeviceData
{
   ::uint32_t r8_rt_upgrade_count = 0;  // creation-order counter for R8G8B8A8_TYPELESS RTs

   // Captured during the motion-vector pass (PS hash 0x923B088C, t0 = scene depth).
   com_ptr<ID3D11Resource>           captured_depth;
   com_ptr<ID3D11ShaderResourceView> captured_depth_srv;
};

class DyingLightGame final : public Game
{
   static DyingLightDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<DyingLightDeviceData*>(device_data.game);
   }

public:
   bool FilterUpgradeResource(const reshade::api::resource_desc& desc, DeviceData& device_data, bool has_initial_data) override
   {
      auto& gdd = GetGameDeviceData(device_data);

      if (desc.texture.format == reshade::api::format::r8g8b8a8_typeless
          && (desc.usage & reshade::api::resource_usage::render_target) != reshade::api::resource_usage::undefined)
      {
         // 256x1 LUT — always upgrade, no index needed
         if (desc.texture.width == 256 && desc.texture.height == 1)
            return true;

         // TAA History: allow only creation indices 2, 3, 4 (0-based)
         ::uint32_t idx = gdd.r8_rt_upgrade_count++;
         return idx == 2 || idx == 3 || idx == 4;
      }

      return true;
   }

   void OnInit(bool async) override
   {
      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12;

#if ENABLE_SR
      sr_game_tooltip =
         "Set Anti-Aliasing > On "
         "in the game's Video options for DLAA to engage.";
#endif

      GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('1');
      GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('0');
      GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('2');
   }

   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.game = new DyingLightDeviceData;

#if ENABLE_SR
      D3D11_TEXTURE2D_DESC exp_desc  = {};
      exp_desc.Width                 = 1;
      exp_desc.Height                = 1;
      exp_desc.MipLevels             = 1;
      exp_desc.ArraySize             = 1;
      exp_desc.Format                = DXGI_FORMAT_R32_FLOAT;
      exp_desc.SampleDesc.Count      = 1;
      exp_desc.Usage                 = D3D11_USAGE_IMMUTABLE;
      exp_desc.BindFlags             = D3D11_BIND_SHADER_RESOURCE;

      D3D11_SUBRESOURCE_DATA exp_data = {};
      exp_data.pSysMem                = &device_data.sr_exposure_texture_value;
      exp_data.SysMemPitch            = 32;
      exp_data.SysMemSlicePitch       = 32;

      device_data.sr_exposure = nullptr;
      HRESULT hr = native_device->CreateTexture2D(&exp_desc, &exp_data, &device_data.sr_exposure);
      assert(SUCCEEDED(hr));
#endif
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(
      ID3D11Device*                                    native_device,
      ID3D11DeviceContext*                             native_device_context,
      CommandListData&                                 cmd_list_data,
      DeviceData&                                      device_data,
      reshade::api::shader_stage                       stages,
      const ShaderHashesList<OneShaderPerPipeline>&    original_shader_hashes,
      bool                                             is_custom_pass,
      bool&                                            updated_cbuffers,
      std::function<void()>*                           original_draw_dispatch_func) override
   {
      auto& gdd = GetGameDeviceData(device_data);

      // The motion-vector pass (PS 0x923B088C) has the depth buffer in t0
      if (original_shader_hashes.Contains(shader_hashes_MV))
      {
         com_ptr<ID3D11ShaderResourceView> mv_srv;
         native_device_context->PSGetShaderResources(0, 1, &mv_srv);
         if (mv_srv)
         {
            com_ptr<ID3D11Resource> res;
            mv_srv->GetResource(&res);
            if (res)
            {
               gdd.captured_depth     = res;
               gdd.captured_depth_srv = mv_srv;
            }
         }
      }

      if (original_shader_hashes.Contains(shader_hashes_TAA))
      {
         has_drawn_taa      = true;
         device_data.taa_detected = true;

#if ENABLE_SR
         if (device_data.sr_type != SR::Type::None && !device_data.sr_suppressed)
         {
            ASSERT_ONCE(!device_data.has_drawn_sr);

            //   t0 = motion vectors
            //   t1 = scene color
            com_ptr<ID3D11ShaderResourceView> ps_srvs[3];
            native_device_context->PSGetShaderResources(0, ARRAYSIZE(ps_srvs), &ps_srvs[0]);

            com_ptr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
            com_ptr<ID3D11DepthStencilView> dsv;
            native_device_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, &rtvs[0], &dsv);

            const bool inputs_valid = ps_srvs[0].get() && ps_srvs[1].get() && rtvs[0].get();
            ASSERT_ONCE(inputs_valid);

            if (inputs_valid)
            {
               DrawStateStack<DrawStateStackType::FullGraphics> draw_state_stack;
               DrawStateStack<DrawStateStackType::Compute>      compute_state_stack;
               draw_state_stack.Cache(native_device_context, device_data.uav_max_count);
               compute_state_stack.Cache(native_device_context, device_data.uav_max_count);

               auto* sr_instance_data = device_data.GetSRInstanceData();
               ASSERT_ONCE(sr_instance_data);

               com_ptr<ID3D11Resource>  output_resource;
               rtvs[0]->GetResource(&output_resource);
               com_ptr<ID3D11Texture2D> output_color;
               HRESULT hr = output_resource->QueryInterface(&output_color);
               ASSERT_ONCE(SUCCEEDED(hr));

               D3D11_TEXTURE2D_DESC taa_out_desc;
               output_color->GetDesc(&taa_out_desc);

               SR::SettingsData settings              = {};
               settings.output_width                  = unsigned int(device_data.output_resolution.x + 0.5f);
               settings.output_height                 = unsigned int(device_data.output_resolution.y + 0.5f);
               settings.render_width                  = unsigned int(device_data.render_resolution.x + 0.5f);
               settings.render_height                 = unsigned int(device_data.render_resolution.y + 0.5f);
               settings.hdr                           = true;
               settings.inverted_depth                = true;
               settings.mvs_jittered                  = true;
               settings.auto_exposure                 = device_data.sr_type != SR::Type::FSR;
               // Motion vectors are UV space so scale to pixel space
               settings.mvs_x_scale                   = -device_data.render_resolution.x;
               settings.mvs_y_scale                   = -device_data.render_resolution.y;
               settings.render_preset                 = dlss_render_preset;
               sr_implementations[device_data.sr_type]->UpdateSettings(sr_instance_data, native_device_context, settings);

               bool skip_sr          = taa_out_desc.Width  < sr_instance_data->min_resolution
                                    || taa_out_desc.Height < sr_instance_data->min_resolution;
               bool sr_output_changed = false;

               const bool dlss_use_native_uav     = true;
               const bool dlss_output_supports_uav = dlss_use_native_uav
                  && (taa_out_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;

               if (!dlss_output_supports_uav)
               {
                  D3D11_TEXTURE2D_DESC out_desc  = taa_out_desc;
                  out_desc.Width                 = std::lrintf(device_data.output_resolution.x);
                  out_desc.Height                = std::lrintf(device_data.output_resolution.y);
                  out_desc.BindFlags            |= D3D11_BIND_UNORDERED_ACCESS;

                  if (device_data.sr_output_color)
                  {
                     D3D11_TEXTURE2D_DESC prev_desc;
                     device_data.sr_output_color->GetDesc(&prev_desc);
                     sr_output_changed = prev_desc.Width  != out_desc.Width
                                      || prev_desc.Height != out_desc.Height
                                      || prev_desc.Format != out_desc.Format;
                  }
                  if (!device_data.sr_output_color || sr_output_changed)
                  {
                     device_data.sr_output_color = nullptr;
                     hr = native_device->CreateTexture2D(&out_desc, nullptr, &device_data.sr_output_color);
                     ASSERT_ONCE(SUCCEEDED(hr));
                  }
                  if (!device_data.sr_output_color)
                     skip_sr = true;
               }
               else
               {
                  ASSERT_ONCE(!device_data.sr_output_color);
                  device_data.sr_output_color = output_color;
               }

               if (!skip_sr)
               {
                  com_ptr<ID3D11Resource> sr_source_color, motion_vectors;
                  ps_srvs[1]->GetResource(&sr_source_color);  // t1: scene color
                  ps_srvs[0]->GetResource(&motion_vectors);   // t0: motion vectors
                  ASSERT_ONCE(sr_source_color && motion_vectors && gdd.captured_depth);

                  depth = gdd.captured_depth;

                  bool reset_sr              = device_data.force_reset_sr || sr_output_changed;
                  device_data.force_reset_sr = false;

                  // these are what the game uses for the near and far plane most of the time
                  float near_plane = 0.05f;
                  float far_plane  = 100000.f;
                  CameraStruct* cam = GetCamera();

                  if (cam)
                  {
                     near_plane = cam->zNear;
                     far_plane  = cam->zFar;
                  }

                  prev_frame_jitters = frame_jitters;
                  if (g_pixel_jitter_x != 0.f || g_pixel_jitter_y != 0.f)
                  {
                     frame_jitters.x = g_pixel_jitter_x;
                     frame_jitters.y = -g_pixel_jitter_y;
                  }
                  else
                  {
                     frame_jitters = float2{0.f, 0.f};
                  }

                  SR::SuperResolutionImpl::DrawData draw_data = {};
                  draw_data.source_color   = sr_source_color.get();
                  draw_data.output_color   = device_data.sr_output_color.get();
                  draw_data.motion_vectors = motion_vectors.get();
                  draw_data.depth_buffer   = depth.get();
                  draw_data.pre_exposure   = 0.f;
                  draw_data.jitter_x       = frame_jitters.x;
                  draw_data.jitter_y       = frame_jitters.y;
                  draw_data.reset          = reset_sr;
                  draw_data.near_plane     = near_plane;
                  draw_data.far_plane      = far_plane;
                  draw_data.vert_fov       = 0.f;
                  draw_data.frame_index    = cb_luma_global_settings.FrameIndex;
                  draw_data.time_delta     = 1.0f / 60.0f;
                  if (!settings.auto_exposure)
                     draw_data.exposure = device_data.sr_exposure.get();

                  bool sr_succeeded = sr_implementations[device_data.sr_type]->Draw(
                     sr_instance_data, native_device_context, draw_data);

                  if (sr_succeeded)
                     device_data.has_drawn_sr = true;
                  else
                  {
                     cb_luma_global_settings.SRType              = 0;
                     device_data.cb_luma_global_settings_dirty   = true;
                     device_data.sr_suppressed                   = true;
                     device_data.force_reset_sr                  = true;
                  }
               }

               draw_state_stack.Restore(native_device_context, device_data.uav_max_count);
               compute_state_stack.Restore(native_device_context, device_data.uav_max_count);

               if (device_data.has_drawn_sr)
               {
                  if (!dlss_output_supports_uav)
                     native_device_context->CopyResource(output_color.get(), device_data.sr_output_color.get());
                  else
                     device_data.sr_output_color = nullptr;

                  return DrawOrDispatchOverrideType::Replaced;
               }

               if (dlss_output_supports_uav)
                  device_data.sr_output_color = nullptr;
            }
            return DrawOrDispatchOverrideType::Skip;
         }
#endif // ENABLE_SR
      }

      return DrawOrDispatchOverrideType::None;
   }

   void UpdateLumaInstanceDataCB(CB::LumaInstanceDataPadded& data, CommandListData& cmd_list_data, DeviceData& device_data) override
   {
      float2 curr = frame_jitters;
      curr.x /= device_data.render_resolution.x;
      curr.y /= device_data.render_resolution.y;
      memcpy(&data.GameData.CurrJitters, &curr, sizeof(curr));

      float2 prev = prev_frame_jitters;
      prev.x /= device_data.render_resolution.x;
      prev.y /= device_data.render_resolution.y;
      memcpy(&data.GameData.PrevJitters, &prev, sizeof(prev));

      float4 res;
      res.x = device_data.render_resolution.x;
      res.y = device_data.render_resolution.y;
      res.z = 1.0f / device_data.render_resolution.x;
      res.w = 1.0f / device_data.render_resolution.y;
      memcpy(&data.GameData.RenderResolution, &res, sizeof(res));

   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      if (!has_drawn_taa)
      {
#if ENABLE_SR
         device_data.force_reset_sr = true;
#endif
         device_data.taa_detected = false;

         if (cb_luma_global_settings.SRType > 0)
         {
            cb_luma_global_settings.SRType              = 0;
            device_data.cb_luma_global_settings_dirty   = true;
         }
         device_data.sr_suppressed = false;
      }

      bool drew_sr = cb_luma_global_settings.SRType > 0;
      cb_luma_global_settings.SRType =
         (device_data.sr_type != SR::Type::None && !device_data.sr_suppressed && device_data.taa_detected)
            ? (uint(device_data.sr_type) + 1)
            : 0;

      if (cb_luma_global_settings.SRType > 0 && !drew_sr)
      {
         device_data.cb_luma_global_settings_dirty = true;
         device_data.force_reset_sr                = true;
      }

      depth                                    = nullptr;
      device_data.has_drawn_main_post_processing = false;
      device_data.has_drawn_sr                   = false;
      has_drawn_taa                              = false;

      // The captured depth and its SRV are only valid for the frame they were grabbed in; release
      // both so we don't hold a stale reference into a resource the engine may resize/destroy.
      auto& gdd_present = GetGameDeviceData(device_data);
      gdd_present.captured_depth     = nullptr;
      gdd_present.captured_depth_srv = nullptr;

#if ENABLE_SR
      if (!custom_texture_mip_lod_bias_offset)
      {
         std::shared_lock shared_lock_samplers(s_mutex_samplers);
         device_data.texture_mip_lod_bias_offset =
            (device_data.sr_type != SR::Type::None && !device_data.sr_suppressed)
               ? SR::GetMipLODBias(device_data.render_resolution.y, device_data.output_resolution.y)
               : 0.f;
      }
#endif

      // frame_jitters is owned by OnDrawOrDispatch (read fresh from the projection
      // matrix every TAA pass). Just keep prev in sync for Luma's CB.
      if (!(device_data.taa_detected
            && device_data.sr_type != SR::Type::None
            && !device_data.sr_suppressed))
      {
         frame_jitters      = float2{0.f, 0.f};
         prev_frame_jitters = float2{0.f, 0.f};
      }
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      reshade::api::effect_runtime* runtime = nullptr;

   }

   void PrintImGuiAbout() override
   {
      ImGui::Text("Dying Light Luma mod - Developed by ...", "");
   }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      std::filesystem::path file_path = System::GetModulePath(hModule);
      std::string file_name = file_path.stem().string();
      const char* game_name = "Dying Light";
      std::string mod_description = "Dying Light Luma mod";

      ::uint32_t mod_version = 1;
      Globals::SetGlobals(game_name, mod_description.c_str(), "https://github.com/Filoppi/Luma-Framework/", mod_version);

      InitCameraHook();
      InitJitterHook();

      swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
      swapchain_upgrade_type = SwapchainUpgradeType::scRGB;
      texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;

      texture_upgrade_formats = {
        reshade::api::format::r8g8b8a8_typeless,
      };

      texture_format_upgrades_2d_size_filters = 0
    | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution
    | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio
    | (uint32_t)TextureFormatUpgrades2DSizeFilters::No1Px;
      
      enable_samplers_upgrade = true;

      shader_hashes_TAA.pixel_shaders = { 0xA67ABF78 };
      shader_hashes_MV.pixel_shaders  = { 0x923B088C };


      game = new DyingLightGame();
   }
   else if (ul_reason_for_call == DLL_PROCESS_DETACH)
   {
      if (cam_hook_memory)
      {
         VirtualFree(cam_hook_memory, 0, MEM_RELEASE);
         cam_hook_memory = nullptr;
      }
      if (jitter_hook_memory)
      {
         VirtualFree(jitter_hook_memory, 0, MEM_RELEASE);
         jitter_hook_memory = nullptr;
      }
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   return TRUE;
}