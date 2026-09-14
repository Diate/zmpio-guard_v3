<#
.SYNOPSIS
    Repository layout and external toolchain locations, for PowerShell callers.

.DESCRIPTION
    Every in-tree path is derived from this module's own location, so the tree
    can be cloned or moved anywhere. Only config/toolchain.env holds absolute
    paths, and an environment variable that is already set always wins over the
    value in that file.

.EXAMPLE
    Import-Module (Join-Path $PSScriptRoot 'ProjectPaths.psm1') -Force
    $p = Get-ZmpioPaths
    $p.VivadoProjectDir
#>

function Get-ZmpioRepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

function Get-ZmpioToolchainEnv {
    $root = Get-ZmpioRepoRoot
    $file = Join-Path $root 'config/toolchain.env'
    $values = @{}
    if (-not (Test-Path $file)) { return $values }
    foreach ($line in Get-Content $file) {
        $trimmed = $line.Trim()
        if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
        $idx = $trimmed.IndexOf('=')
        if ($idx -lt 1) { continue }
        $key = $trimmed.Substring(0, $idx).Trim()
        $val = $trimmed.Substring($idx + 1).Trim()
        $values[$key] = $val
    }
    return $values
}

function Get-ZmpioTool {
    param([Parameter(Mandatory = $true)][string]$Name)
    $fromEnv = [Environment]::GetEnvironmentVariable($Name)
    if ($fromEnv) { return $fromEnv }
    $cfg = Get-ZmpioToolchainEnv
    if ($cfg.ContainsKey($Name)) { return $cfg[$Name] }
    return $null
}

function Get-ZmpioPaths {
    $root = Get-ZmpioRepoRoot
    $hardware = Join-Path $root 'hardware'
    $firmware = Join-Path $root 'firmware'
    return [pscustomobject]@{
        RepoRoot          = $root
        ConfigDir         = Join-Path $root 'config'
        CommonDir         = Join-Path $root 'common'
        DocsDir           = Join-Path $root 'docs'
        FirmwareDir       = $firmware
        Cpu0AppDir        = Join-Path $firmware 'cpu0_application'
        Cpu1AppDir        = Join-Path $firmware 'app_freertos'
        PlatformDir       = Join-Path $firmware 'platform_dual'
        HardwareDir       = $hardware
        RtlDir            = Join-Path $hardware 'rtl'
        VivadoProjectDir  = Join-Path $hardware 'vivado/project_hub'
        LinuxSwDir        = Join-Path $root 'software/linux'
        DspHostSimDir     = Join-Path $root 'dsp_host_sim'
        DeployDir         = Join-Path $root 'deploy/petalinux_overlay'
        ScriptsDir        = Join-Path $root 'scripts'
        ToolsDir          = Join-Path $root 'tools'
        ArtifactsDir      = Join-Path $root 'artifacts'
        OutputDir         = Join-Path $root 'output'
    }
}

Export-ModuleMember -Function Get-ZmpioRepoRoot, Get-ZmpioToolchainEnv, Get-ZmpioTool, Get-ZmpioPaths
