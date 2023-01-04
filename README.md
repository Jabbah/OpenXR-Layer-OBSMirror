# OpenXR OBS Mirror

An OpenXR API Layer to mirror what you see in VR to OBS. (Currently DX11 support only)

OpenXR Layer API template was used as the basis for this and can be found here:
https://github.com/mbucchia/OpenXR-Layer-Template

Note: For smooth recording in OBS run as Admin:
https://obsproject.com/forum/threads/obs-studio-24-0-3-gpu-priority-fix-testing.111669/

# Installation instructions:

## Install the OpenXR Layer
Get the latest release from the releases section on the right.

Extract the files to a folder in Program Files e.g. C:\Program Files\OpenXR-OBSMirror\

You may need to unblock the files by right click > properties > check unblock

<img width="269" alt="image" src="https://user-images.githubusercontent.com/2940221/210623325-e47bb303-e3d0-427a-abaa-845a2fd116eb.png">

If you have isntalled a previous version you should uninstall that first.

To install the OpenXR Layer right click on Install-Layer.ps1 and select "Run with PowerShell"

To uninstall OpenXR Layer right click on Uninstall-Layer.ps1 and select "Run with PowerShell"


## Install the OBS plugin:
Copy the files win-openxr.dll and win-openxr.pdb to your OBS plugins dir e.g. C:\Program Files\obs-studio\obs-plugins\64bit

The plugin should appear in the OBS sources list:

<img width="170" alt="image" src="https://user-images.githubusercontent.com/2940221/210623787-e66728e4-c92d-476e-9ad3-82028c0d2a1c.png">
