$RegistryPath = "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
$currentDir = if (!$PSScriptRoot) {"$pwd"} else {"$PSScriptRoot"};
$JsonPath = Join-Path "$currentDir" "XR_APILAYER_NOVENDOR_OBSMirror.json"

$OldKeyPath = 'HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit\[*]XR_APILAYER_NOVENDOR_OBSMirror.json'

Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
	& {
		If (-not (Test-Path $RegistryPath)) {
			New-Item -Path $RegistryPath -Force | Out-Null
		}
		Remove-ItemProperty -Path $RegistryPath -Name "*XR_APILAYER_NOVENDOR_OBSMirror.json" -Force | Out-Null
		New-ItemProperty -Path $RegistryPath -Name '$jsonPath' -PropertyType DWord -Value 0 -Force | Out-Null
	}
"@
