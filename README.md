# Universal-Dear-ImGui-Hook
An universal Dear ImGui Hook for Directx12 D3D12 (D3D11, D3D10 and maybe Vulkan will be added later)

# Important

[19/08/2025] The project is actually stable for directx12, I must fix some potential issues and add the others directx engines but you can actually use it, I tested few games and applications without issues but you can debug it with DebugView if you have some problems, I added logs to directly know what is wrong.

Here is the last stable commit: https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/commit/ca3d843057ff8647f40a94ba46034a7cca8b2084

## Getting Started

- To use it, you need to compile it and inject your dll file in an application process.
- If you have an error about `libMinHook.x64.lib`, recompile https://github.com/TsudaKageyu/minhook in x64 and replace the old .lib file by your new one.
- If you have some issues with the `vulkan-1.lib` download the lastest Vulkan SDK and replace the existing file by the new one from the SDK's lib directory `ex: C:\VulkanSDK\YOURVERSION\Lib\vulkan-1.lib`

![alt text](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## Built With

* [ImGui](https://github.com/ocornut/imgui) - Dear ImGui: Bloat-free Immediate Mode Graphical User interface for C++ with minimal dependencies
* [MinHook](https://github.com/TsudaKageyu/minhook) - The Minimalistic x86/x64 API Hooking Library for Windows

## Authors

* **Rebzzel** - *Initial work about the kiero library (no more used in this project)* - [Rebzzel](https://github.com/Rebzzel)
* **Sh0ckFR** | **Revan600** - *Updated version with ImGui + InputHook* - [Sh0ckFR](https://github.com/Sh0ckFR) - [Revan600](https://github.com/Revan600)

## Contributors

* **primemb** - *Fixed some bugs* - [primemb](https://github.com/primemb)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details







