# Universal-Dear-ImGui-Hook
An universal Dear ImGui Hook for Directx12, 11, 10, 9 and Vulkan.

## Getting Started

- Do not forget to select your engine in `globals.cpp`: https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/blob/master/globals.cpp#L13
- To use it, you need to compile it and inject your dll file in an application process.
- Press `INSERT` to display the menu, `F11` to uninject.
- If you have an error about `libMinHook.x64.lib`, recompile https://github.com/TsudaKageyu/minhook in x64 and replace the old .lib file by your new one.
- If you have some issues with the `vulkan-1.lib` download the lastest Vulkan SDK and replace the existing file by the new one from the SDK's lib directory `ex: C:\VulkanSDK\YOURVERSION\Lib\vulkan-1.lib`

![alt text](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## If you want to debug it

- You can enable the logs here: https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/blob/master/globals.cpp#L15
- Use DebugView to see the different logs during the injection: https://learn.microsoft.com/fr-fr/sysinternals/downloads/debugview

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










