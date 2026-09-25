param(
    [Parameter(Mandatory = $true)]
    [string] $StageDir,

    [Parameter(Mandatory = $true)]
    [string] $ArchivePath
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression

$stageRoot = [System.IO.Path]::GetFullPath($StageDir).TrimEnd([char[]]"\/")
$dataRoot = Join-Path -Path $stageRoot -ChildPath 'Data'
if (-not (Test-Path -LiteralPath $dataRoot -PathType Container)) {
    throw "Staged Data directory is missing: $dataRoot"
}

$files = @(
    Get-ChildItem -LiteralPath $dataRoot -File -Recurse | ForEach-Object {
        # Package the Data directory's contents: F4SE, MCM, and Textures
        # belong directly at the ZIP root for mod-manager installation.
        $relative = $_.FullName.Substring($dataRoot.Length + 1).Replace('\', '/')
        [pscustomobject]@{
            Relative = $relative
            FullName = $_.FullName
        }
    } | Sort-Object -Property Relative -CaseSensitive
)

if ($files.Count -ne 83) {
    throw "Expected 83 staged files (DLL + 2 MCM + 80 DDS), found $($files.Count)"
}

$fixedTimestamp = [DateTimeOffset]::new(
    2020, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$stream = [System.IO.File]::Open(
    $ArchivePath,
    [System.IO.FileMode]::Create,
    [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::None)

try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $stream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        foreach ($file in $files) {
            # ZipArchive produces stable deflate output for a fixed .NET
            # toolchain; the sorted inputs and normalized entry metadata remove
            # the usual per-run sources of nondeterminism.
            $entry = $archive.CreateEntry(
                $file.Relative,
                [System.IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $fixedTimestamp
            $entry.ExternalAttributes = 0

            $input = [System.IO.File]::OpenRead($file.FullName)
            try {
                $output = $entry.Open()
                try {
                    $input.CopyTo($output)
                }
                finally {
                    $output.Dispose()
                }
            }
            finally {
                $input.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    $stream.Dispose()
}
