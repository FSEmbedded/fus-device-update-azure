Import-Module ./AduUpdate.psm1

$updateId = New-AduUpdateId -Provider FUS -Name IMX8MM-Firmware -Version 1.0

$compat = New-AduUpdateCompatibility -Properties @{ deviceManufacturer = 'FUS'; deviceModel = 'IMX8MM' }

$installStep = New-AduInstallationStep -Handler 'fus/firmware:1' -HandlerProperties @{ installedCriteria = '20220411' } -Files 'rauc_update_emmc.artifact'

$update = New-AduImportManifest -UpdateId $updateId -Compatibility $compat -InstallationSteps $installStep

# Write the import manifest to a file, ideally next to the update file(s).
$update | Out-File "./$($updateId.provider).$($updateId.name).$($updateId.version).importmanifest.json" -Encoding utf8
