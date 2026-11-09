<#
  Signs and installs the driver packages built by GitHub Actions.
  Run from an ADMINISTRATOR PowerShell:

    powershell -ExecutionPolicy Bypass -File scripts\install-drivers.ps1 -PackageDir <folder with sklhdaudbus\ and da7219\>

  What it does:
    1. Creates a local test-signing certificate (first run only). The private key
       never leaves this machine and cannot be exported.
    2. Trusts that certificate on this machine (Root + TrustedPublisher).
    3. Signs each .sys and .cat with it.
    4. Tries to create a System Restore point.
    5. Installs each package with pnputil.
#>
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [string[]]$Drivers = @('sklhdaudbus', 'da7219')
)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an Administrator PowerShell.'
}

$subject = 'CN=Chromebook Audio Local Test Signing'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $subject | Select-Object -First 1
if (-not $cert) {
    Write-Host 'Creating local test-signing certificate...'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\LocalMachine\My -KeyExportPolicy NonExportable `
        -NotAfter (Get-Date).AddYears(5)
}
foreach ($storeName in 'Root', 'TrustedPublisher') {
    $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
    $store.Open('ReadWrite')
    if (-not ($store.Certificates | Where-Object Thumbprint -eq $cert.Thumbprint)) {
        $store.Add([Security.Cryptography.X509Certificates.X509Certificate2]::new($cert.RawData))
        Write-Host "Trusted certificate in LocalMachine\$storeName"
    }
    $store.Close()
}

foreach ($d in $Drivers) {
    $dir = Join-Path $PackageDir $d
    if (-not (Test-Path $dir)) { throw "Missing package folder: $dir" }
    foreach ($f in Get-ChildItem $dir -Include *.sys, *.cat -Recurse) {
        $r = Set-AuthenticodeSignature -FilePath $f.FullName -Certificate $cert -HashAlgorithm SHA256
        Write-Host ("Signed {0}: {1}" -f $f.Name, $r.Status)
        if ($r.Status -ne 'Valid') { throw "Signing failed for $($f.FullName): $($r.StatusMessage)" }
    }
}

try {
    Checkpoint-Computer -Description 'Before Chromebook audio driver install' -RestorePointType DEVICE_DRIVER_INSTALL
    Write-Host 'Restore point created.'
} catch {
    Write-Warning "Could not create a restore point: $($_.Exception.Message)"
}

foreach ($d in $Drivers) {
    $inf = Get-ChildItem (Join-Path $PackageDir $d) -Filter *.inf -Recurse | Select-Object -First 1
    Write-Host "Installing $($inf.FullName)"
    pnputil /add-driver $inf.FullName /install
}

Write-Host ''
Write-Host 'Done. Reboot, then ask Claude to re-scan the devices.'
