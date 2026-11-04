#!/usr/bin/env pwsh
# Tao spec-kit feature tu design doc co san trong Virtual_TCU_Lab_30_System_Designs.
# Doc goc duoc copy thanh spec.md => khong can chay /speckit-specify.
param([Parameter(Mandatory)][int]$Project)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$doc = Get-ChildItem (Join-Path $root 'Virtual_TCU_Lab_30_System_Designs') -Filter ('{0:00}_*.md' -f $Project) |
    Select-Object -First 1
if (-not $doc) { throw "Khong tim thay design doc cho project $Project" }

$short = ($doc.BaseName -replace '^\d+_', '' -replace '_System_Design$', '' -replace '_', '-').ToLowerInvariant()
$dir = Join-Path $root ('specs/{0:000}-{1}' -f $Project, $short)
New-Item -ItemType Directory -Path $dir -Force | Out-Null

$spec = Join-Path $dir 'spec.md'
if (Test-Path -PathType Leaf $spec) {
    Write-Host "spec.md da ton tai, giu nguyen: $spec"
} else {
    Copy-Item -LiteralPath $doc.FullName -Destination $spec
    Write-Host "spec.md <- $($doc.Name)"
}

& (Join-Path $PSScriptRoot 'scripts/powershell/create-new-feature.ps1') `
    -Number $Project -ShortName $short -AllowExistingBranch $doc.BaseName
