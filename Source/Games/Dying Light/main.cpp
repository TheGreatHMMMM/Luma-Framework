#define GAME_DYING_LIGHT 1

#define CHECK_GRAPHICS_API_COMPATIBILITY 1

// DRM (Steam) may interfere with auto-debugger attachment
#define DISABLE_AUTO_DEBUGGER 1

#define ENABLE_NGX 1

#include "../../../Shaders/Dying Light/Includes/GameCBuffers.hlsl"
#include "..\..\Core\core.hpp"

namespace
{
   std::set<reshade::api::format> toggleable_texture_upgrade_formats;

   ShaderHashesList shader_hashes_TAA;             // 0xA67ABF78 – SMAA T2X pass

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

   uintptr_t jitterTableAddr = 0;
   float     originalJitterTable[4] = {};
   bool      jitterTableOverridden  = false;

   CameraStruct* GetCamera()
   {
      if (!camStructBase)
         return nullptr;
      return reinterpret_cast<CameraStruct*>(camStructBase + 0x7CE0);
   }

   // ended up having to patch the jitter table instead of going through the projection matrix due to flickering
   void PatchJitterTable()
   {
      if (!jitterTableAddr) return;
      float* table = reinterpret_cast<float*>(jitterTableAddr);
      if (!jitterTableOverridden)
      {
         memcpy(originalJitterTable, table, sizeof(originalJitterTable));
         jitterTableOverridden = true;
      }
      table[0] = frame_jitters.x;  // slot 0 X
      table[1] = frame_jitters.y;  // slot 0 Y
      table[2] = frame_jitters.x;  // slot 1 X
      table[3] = frame_jitters.y;  // slot 1 Y
   }

   void RestoreJitterTable()
   {
      if (!jitterTableAddr || !jitterTableOverridden) return;
      float* table = reinterpret_cast<float*>(jitterTableAddr);
      memcpy(table, originalJitterTable, sizeof(originalJitterTable));
      jitterTableOverridden = false;
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

      jitterTableAddr = baseAddr + 0xA14C90;

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
} // namespace

struct DyingLightDeviceData final : public GameDeviceData
{
};

class DyingLightGame final : public Game
{
   static DyingLightDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<DyingLightDeviceData*>(device_data.game);
   }

public:
   void OnInit(bool async) override
   {
      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12;

#if ENABLE_SR
      sr_game_tooltip =
         "Set Anti-Aliasing > On "
         "in the game's Video options for DLAA to engage.";
#endif
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
               settings.hdr                           = false;
               settings.inverted_depth                = false;
               settings.mvs_jittered                  = false;
               settings.auto_exposure                 = device_data.sr_type != SR::Type::FSR;
               // Motion vectors are UV space (?) so scale to pixel space
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
                  ASSERT_ONCE(sr_source_color && motion_vectors);

                  depth = sr_source_color;

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
            return DrawOrDispatchOverrideType::None;
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

      prev_frame_jitters = frame_jitters;
      if (device_data.taa_detected
         && device_data.sr_type != SR::Type::None
         && !device_data.sr_suppressed)
      {
         const ::uint32_t next_frame = (cb_luma_global_settings.FrameIndex + 1) % 8;
         frame_jitters.x = SR::HaltonSequence(next_frame, 2);
         frame_jitters.y = SR::HaltonSequence(next_frame, 3);
      }
      else
      {
         frame_jitters = float2{0.f, 0.f};
      }

      // only patch the jitter when dlaa is active
      if (device_data.sr_type != SR::Type::None && !device_data.sr_suppressed)
         PatchJitterTable();
      else
         RestoreJitterTable();
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      reshade::api::effect_runtime* runtime = nullptr;

      ImGui::NewLine();

      if (swapchain_format_upgrade_type > TextureFormatUpgradesType::None)
      {
         if (swapchain_format_upgrade_type == TextureFormatUpgradesType::AllowedEnabled ? ImGui::Button("Disable Swapchain Upgrade") : ImGui::Button("Enable Swapchain Upgrade"))
         {
            swapchain_format_upgrade_type = swapchain_format_upgrade_type == TextureFormatUpgradesType::AllowedEnabled ? TextureFormatUpgradesType::AllowedDisabled : TextureFormatUpgradesType::AllowedEnabled;
         }
      }
      if (texture_format_upgrades_type > TextureFormatUpgradesType::None)
      {
         if (texture_format_upgrades_type == TextureFormatUpgradesType::AllowedEnabled ? ImGui::Button("Disable Texture Format Upgrades") : ImGui::Button("Enable Texture Format Upgrades"))
         {
            texture_format_upgrades_type = texture_format_upgrades_type == TextureFormatUpgradesType::AllowedEnabled ? TextureFormatUpgradesType::AllowedDisabled : TextureFormatUpgradesType::AllowedEnabled;
         }

         ImGui::NewLine();
         ImGui::Text("Texture Format Upgrades:");
         for (auto toggleable_texture_upgrade_format : toggleable_texture_upgrade_formats)
         {
            // Dumb stream conversion
            std::ostringstream oss;
            oss << toggleable_texture_upgrade_format;
            std::string toggleable_texture_upgrade_format_name = oss.str();

            bool enabled = texture_upgrade_formats.contains(toggleable_texture_upgrade_format);

            int mode = enabled ? 1 : 0;
            const char* settings_name_strings[2] = { "Off", "On" };
            if (ImGui::SliderInt(toggleable_texture_upgrade_format_name.c_str(), &mode, 0, 1, settings_name_strings[mode], ImGuiSliderFlags_NoInput))
            {
               if (mode >= 1)
               {
                  texture_upgrade_formats.emplace(toggleable_texture_upgrade_format);
               }
               else
               {
                  texture_upgrade_formats.erase(toggleable_texture_upgrade_format);
               }
            }
         }
      }

      ImGui::NewLine();
      if (prevent_fullscreen_state ? ImGui::Button("Allow Fullscreen State") : ImGui::Button("Disallow Fullscreen State"))
      {
         prevent_fullscreen_state = !prevent_fullscreen_state;
      }
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

      //swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
      //swapchain_upgrade_type = SwapchainUpgradeType::scRGB;
      //texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;

      // ### Check which of these are needed and remove the rest ###
      texture_upgrade_formats = {
        // reshade::api::format::r8g8b8a8_unorm,
        // reshade::api::format::r8g8b8a8_unorm_srgb,
        // reshade::api::format::r8g8b8a8_typeless,
        // reshade::api::format::r11g11b10_float,
      };
      //texture_format_upgrades_lut_size = 256;
      //texture_format_upgrades_lut_dimensions = LUTDimensions::_1D;
      texture_format_upgrades_2d_size_filters = 0 | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio;

      enable_samplers_upgrade = true;

      shader_hashes_TAA.pixel_shaders = { 0xA67ABF78 };

      game = new DyingLightGame();
   }
   else if (ul_reason_for_call == DLL_PROCESS_DETACH)
   {
      if (cam_hook_memory)
      {
         VirtualFree(cam_hook_memory, 0, MEM_RELEASE);
         cam_hook_memory = nullptr;
      }
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   return TRUE;
}