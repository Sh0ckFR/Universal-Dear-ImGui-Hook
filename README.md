# Universal-Dear-ImGui-Hook
An universal Dear ImGui Hook for Directx12 D3D12 (D3D11, D3D10 and maybe Vulkan will be added later)

# Important

[19/08/2025] The project is actually stable for directx12, I must fix some potential issues and add the others directx engines but you can actually use it, I tested few games and applications without issues but you can debug it with DebugView if you have some problems, I added logs to directly know what is wrong.

## Getting Started

- This project is based on https://github.com/Rebzzel/kiero
- To use it, you need to compile it and inject your dll file in an application process.
- If you have an error about `libMinHook.x64.lib`, recompile https://github.com/TsudaKageyu/minhook in x64 and replace the old .lib file by your new one.

![alt text](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## Built With

* [ImGui](https://github.com/ocornut/imgui) - Dear ImGui: Bloat-free Immediate Mode Graphical User interface for C++ with minimal dependencies
* [Kiero](https://github.com/Rebzzel/kiero) - Kiero: Universal graphical hook for a D3D9-D3D12, OpenGL and Vulkan based games.
* [MinHook](https://github.com/TsudaKageyu/minhook) - The Minimalistic x86/x64 API Hooking Library for Windows

## Authors

* **Rebzzel** - *Initial work* - [Rebzzel](https://github.com/Rebzzel)
* **Sh0ckFR** | **Revan600** - *Updated version with ImGui + InputHook* - [Sh0ckFR](https://github.com/Sh0ckFR) - [Revan600](https://github.com/Revan600)

## Contributors

* **primemb** - *Fixed some bugs, active member* - [primemb](https://github.com/primemb)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details

The original licence of the Kiero library can be found here: [LICENSE](https://github.com/Rebzzel/kiero/blob/master/LICENSE)




