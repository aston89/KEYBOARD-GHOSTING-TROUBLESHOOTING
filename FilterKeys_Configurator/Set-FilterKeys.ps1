
param(
    [ValidateSet('OFF','READ','REPEAT','BOUNCE')]
    [string]$Mode = 'READ',

    [int]$WaitMs = 0,
    [int]$DelayMs = 250,
    [int]$RepeatMs = 50,
    [int]$BounceMs = 32
)

$ErrorActionPreference = 'Stop'

# ==================================================
# VALIDATION
# ==================================================

foreach ($v in @($WaitMs,$DelayMs,$RepeatMs,$BounceMs)) {
    if ($v -lt 0 -or $v -gt 20000) {
        throw "Values must be between 0 and 20000 ms."
    }
}

if ($Mode -eq 'REPEAT') {
    if ($DelayMs -lt 1 -or $RepeatMs -lt 1 -or $BounceMs -ne 0) {
        throw "Invalid REPEAT configuration."
    }
    # WaitMs = 0 IS ALLOWED.
}

if ($Mode -eq 'BOUNCE') {
    if ($WaitMs -ne 0 -or $DelayMs -ne 0 -or
        $RepeatMs -ne 0 -or $BounceMs -lt 1) {
        throw "Invalid BOUNCE configuration."
    }
}

# ==================================================
# WINDOWS API
# ==================================================

if (-not ('FKInteractiveApi' -as [type])) {

    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)]
public struct FKInteractiveStruct {
    public uint cbSize;
    public uint dwFlags;
    public uint iWaitMSec;
    public uint iDelayMSec;
    public uint iRepeatMSec;
    public uint iBounceMSec;
}

public static class FKInteractiveApi {

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SystemParametersInfo(
        uint action,
        uint param,
        ref FKInteractiveStruct data,
        uint flags
    );
}
'@
}

# ==================================================
# READ FUNCTION
# ==================================================

function Get-FKConfiguration {

    $s = New-Object FKInteractiveStruct
    $s.cbSize = 24

    $ok = [FKInteractiveApi]::SystemParametersInfo(
        0x32, 24, [ref]$s, 0
    )

    if (-not $ok) {
        throw "SPI_GETFILTERKEYS failed."
    }

    return $s
}

function Show-FKConfiguration($s) {

    [pscustomobject]@{
        FilterKeysON = [bool]($s.dwFlags -band 0x01)
        AudibleClick = [bool]($s.dwFlags -band 0x40)
        Flags        = '0x{0:X8}' -f $s.dwFlags
        Wait_ms      = $s.iWaitMSec
        Delay_ms     = $s.iDelayMSec
        Repeat_ms    = $s.iRepeatMSec
        Bounce_ms    = $s.iBounceMSec
    } | Format-List
}

# ==================================================
# INITIAL STATE
# ==================================================

$original = Get-FKConfiguration

Write-Host "`nCURRENT CONFIGURATION:" -ForegroundColor Cyan
Show-FKConfiguration $original

if ($Mode -eq 'READ') {
    exit 0
}

# ==================================================
# BUILD TARGET CONFIGURATION
# ==================================================

$target = $original

switch ($Mode) {

    'OFF' {

        # Disable only FilterKeys.
        # Preserve accessibility flags and timing values.
        $target.dwFlags = [uint32](
            $target.dwFlags -band 0xFFFFFFFE
        )
    }

    'REPEAT' {

        # Enable FilterKeys, preserve CLICKON and other flags.
        $target.dwFlags = [uint32](
            $target.dwFlags -bor 0x01
        )

        $target.iWaitMSec   = [uint32]$WaitMs
        $target.iDelayMSec  = [uint32]$DelayMs
        $target.iRepeatMSec = [uint32]$RepeatMs
        $target.iBounceMSec = 0
    }

    'BOUNCE' {

        $target.dwFlags = [uint32](
            $target.dwFlags -bor 0x01
        )

        $target.iWaitMSec   = 0
        $target.iDelayMSec  = 0
        $target.iRepeatMSec = 0
        $target.iBounceMSec = [uint32]$BounceMs
    }
}

# ==================================================
# APPLY RUNTIME
# ==================================================

Write-Host "`nApplying $Mode configuration..." -ForegroundColor Yellow

$ok = [FKInteractiveApi]::SystemParametersInfo(
    0x33, 24, [ref]$target, 0
)

if (-not $ok) {

    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()

    Write-Host "Windows rejected the configuration. Win32 error: $err" -ForegroundColor Red
    exit 1
}

# ==================================================
# VERIFY RUNTIME
# ==================================================

$actual = Get-FKConfiguration

$matches = (
    $actual.dwFlags        -eq $target.dwFlags -and
    $actual.iWaitMSec      -eq $target.iWaitMSec -and
    $actual.iDelayMSec     -eq $target.iDelayMSec -and
    $actual.iRepeatMSec    -eq $target.iRepeatMSec -and
    $actual.iBounceMSec    -eq $target.iBounceMSec
)

if (-not $matches) {

    Write-Host "`nRUNTIME MISMATCH. Restoring original configuration..." -ForegroundColor Red

    $rollback = $original

    [FKInteractiveApi]::SystemParametersInfo(
        0x33, 24, [ref]$rollback, 0
    ) | Out-Null

    Write-Host "Original runtime configuration restored."
    exit 2
}

# ==================================================
# PERSIST TO USER PROFILE
# ==================================================

# SPIF_UPDATEINIFILE = 1
# SPIF_SENDCHANGE    = 2

$persist = $actual

$ok = [FKInteractiveApi]::SystemParametersInfo(
    0x33, 24, [ref]$persist, 3
)

if (-not $ok) {

    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()

    Write-Host "Persistence failed. Restoring original configuration." -ForegroundColor Red

    $rollback = $original

    [FKInteractiveApi]::SystemParametersInfo(
        0x33, 24, [ref]$rollback, 3
    ) | Out-Null

    Write-Host "Win32 error: $err"
    exit 3
}

# ==================================================
# FINAL VERIFICATION
# ==================================================

$final = Get-FKConfiguration

$finalMatches = (
    $final.dwFlags        -eq $target.dwFlags -and
    $final.iWaitMSec      -eq $target.iWaitMSec -and
    $final.iDelayMSec     -eq $target.iDelayMSec -and
    $final.iRepeatMSec    -eq $target.iRepeatMSec -and
    $final.iBounceMSec    -eq $target.iBounceMSec
)

if (-not $finalMatches) {
    throw "Final readback mismatch after persistence."
}

Write-Host "`nFINAL CONFIGURATION:" -ForegroundColor Cyan
Show-FKConfiguration $final

Write-Host "Configuration successfully applied and saved." -ForegroundColor Green
exit 0
