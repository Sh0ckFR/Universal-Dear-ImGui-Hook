# Universal-Dear-ImGui-Hook
An universal Dear ImGui Hook for Directx12 D3D12 (D3D11, D3D10 and maybe Vulkan will be added later)

# Important

[24/04/2025] I'm actually doing a full rework of this code so it will not work actually, it will be stable in fews days or the next weeks, I will edit this message when everything will be stable.

## Getting Started

- This project is based on https://github.com/Rebzzel/kiero
- To use it, you need to compile it and inject your dll file in an application process.
- If you have an error about `libMinHook.x64.lib`, recompile https://github.com/TsudaKageyu/minhook in x64 and replace the old .lib file by your new one.

![alt text](https://raw.githubusercontent.com/Sh0ckFR/Universal-Dear-ImGui-Hook/master/imgui.png)

## Built With

* [ImGui](https://github.com/ocornut/imgui) - Dear ImGui: Bloat-free Immediate Mode Graphical User interface for C++ with minimal dependencies
* [Kiero](https://github.com/Rebzzel/kiero) - Kiero: Universal graphical hook for a D3D9-D3D12, OpenGL and Vulcan based games.
* [MinHook](https://github.com/TsudaKageyu/minhook) - The Minimalistic x86/x64 API Hooking Library for Windows

## Authors

* **Rebzzel** - *Initial work* - [Rebzzel](https://github.com/Rebzzel)
* **Sh0ckFR** | **Revan600** - *Updated version with ImGui + InputHook* - [Sh0ckFR](https://github.com/Sh0ckFR) - [Revan600](https://github.com/Revan600)

## Contributors

* **primemb** - *Fixed some bugs, active member* - [primemb](https://github.com/primemb)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details

The original licence of the Kiero library can be found here: [LICENSE](https://github.com/Rebzzel/kiero/blob/master/LICENSE)
