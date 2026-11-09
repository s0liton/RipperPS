# RipperPS

The native PS5 app for ripper (`PPSA76700`): the drives and the disc in each, the confirm sheet, the rip queue and the settings, on the TV with the controller. It talks to the ripper payload on the console (its local API on port 8790), and starts the payload it carries when it isn't running.

It shares RommPS's scaffolding: mihawk-99's [PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate) (at `b577e950`), Sascha Willems' Vulkan example base class with its PS5 hooks, RADV for the GPU, and BlackBearReloaded's UI kit drawn with Vulkan through mihawk-99's PS5_VKHomebrewUI. The app itself is `examples/ripper/ripper.cpp` and the screens in `examples/ripper/kit/`.

## Building and running

It builds against mihawk-99's PS5 stack, checked out in `../ps5-stack` beside this repository (PS5_Vulkan, the payload SDK fork, PS5_Mesa), inside the Docker image from `platform/ps5/app/docker`. Build the payload first (`make ps5`), since the app carries it. From the repository's root:

```bash
tools/ps5-app-build.sh bash -c 'cd /repo/platform/ps5/app/ripper && bash ps5/tools/build.sh ripper'   # dist/PPSA76700/
tools/ps5-app-build.sh bash -c 'cd /repo/platform/ps5/app/ripper && bash ps5/tools/run.sh ripper'     # deploy and a test run
```

## Licences

The UI kit is GPL-3.0-or-later, and so is RipperPS as distributed: its screens, the UI module (`ps5/ui/`) and the kit's sounds (`ps5/licenses/GPL-3.0.txt`). The kit's fonts are OFL-1.1 and Bitstream Vera. The rest of the template is MIT (`LICENSE.md`: Sascha Willems', and mihawk-99's PS5 layer). Every asset and its licence is listed in `ps5/ASSETS.md`, and the built app carries all of it in `LEGAL.txt` and `licenses/`.
