$ErrorActionPreference = 'Stop'
$plan = Get-Content -LiteralPath "$PSScriptRoot\.backups\sd-repair-20260907\plan.json" -Raw | ConvertFrom-Json
$cardRoot = [IO.Path]::GetFullPath('E:\')
$backupRoot = [IO.Path]::GetFullPath($plan.backup)
if ($plan.target -ne 'E:/') { throw 'Unexpected SD target' }
function CardPath([string]$relative) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $cardRoot $relative))
    if (-not $resolved.StartsWith($cardRoot, [StringComparison]::OrdinalIgnoreCase) -or $resolved -eq $cardRoot) { throw 'Path outside SD card' }
    return $resolved
}
$archived = @()
foreach ($item in $plan.archive) {
    $sourceFile = CardPath $item.relative
    if ((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash -ne $item.sha256) { throw "File changed since inspection: $sourceFile" }
    if ($item.replacement -and -not (Test-Path -LiteralPath (CardPath $item.replacement) -PathType Leaf)) { throw 'Replacement app is missing' }
    $backupFile = [IO.Path]::GetFullPath((Join-Path $backupRoot $item.relative))
    if (-not $backupFile.StartsWith($backupRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside backup' }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($backupFile)) -Force | Out-Null
    Copy-Item -LiteralPath $sourceFile -Destination $backupFile
    if ((Get-FileHash -LiteralPath $backupFile -Algorithm SHA256).Hash -ne $item.sha256) { throw 'Backup verification failed' }
    Remove-Item -LiteralPath $sourceFile
    $archived += $item.relative
}
# Update only exact favorite paths pointing to archived app duplicates.
$favorites = CardPath 'favorites.txt'
if (Test-Path -LiteralPath $favorites) {
    $before = [IO.File]::ReadAllText($favorites)
    $after = $before
    foreach ($item in $plan.archive) {
        if ($item.replacement) {
            $oldPath = '/ext/' + $item.relative.Replace('\','/')
            $newPath = '/ext/' + $item.replacement.Replace('\','/')
            $after = $after.Replace($oldPath, $newPath)
        }
    }
    if ($after -ne $before) {
        Copy-Item -LiteralPath $favorites -Destination (Join-Path $backupRoot 'favorites.txt')
        [IO.File]::WriteAllText($favorites, $after, [Text.UTF8Encoding]::new($false))
    }
}
@{ archived = $archived; folders_preserved = $true } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backupRoot 'result.json')
Write-Output "Archived $($archived.Count) duplicate/metadata files. All folders preserved. Backups verified with SHA256."
