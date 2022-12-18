# OpenXR OBS Mirror

An OpenXR API Layer to mirror what you see in VR to OBS. (Currently DX11 support only)

OpenXR Layer API template was used as the basis for this and can be found here:
https://github.com/mbucchia/OpenXR-Layer-Template

# Installation instructions:

## Install the OpenXR Layer
Get the latest release from the releases section on the right.

Extract the files to a folder in Program Files e.g. C:\Program Files\OpenXR-OBSMirror\v0.1.0\

To install the OpenXR Layer right click on Install-Layer.ps1 and select "Run with PowerShell"

To uninstall OpenXR Layer right click on Uninstall-Layer.ps1 and select "Run with PowerShell"


## Install the OBS plugin:
Copy the files win-openxr.dll and win-openxr.pdb to your OBS plugins dir e.g. C:\Program Files\obs-studio\obs-plugins\64bit
