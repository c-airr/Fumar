<#
    Fetches the stock material library into assets/materials.

    The library - ten 2K texture sets from ambientCG, CC0 - lives on its own
    branch, `materials`, so that main stays small to check out. This takes the
    assets/materials folder from that branch and unpacks it into the working
    tree without checking the branch out or touching the index: assets/ is
    git-ignored on main, so nothing shows up as a change.

    Usage:
        tools/get-materials.ps1

    Run it again to update. Then build, or just start the editor: a
    development build reads the library from assets/materials in the source
    tree.
#>

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot

# Plain git output on stderr is progress, not an error; the exit code decides.
$ErrorActionPreference = 'Continue'

git -C $repo fetch origin materials
if ($LASTEXITCODE -ne 0) {
    Write-Error "get-materials: could not fetch the 'materials' branch from origin"
    exit 1
}

# Through a file rather than a pipe: Windows PowerShell re-encodes what passes
# through a pipeline, which corrupts a binary archive.
$archive = Join-Path ([IO.Path]::GetTempPath()) 'fumar-materials.tar'
git -C $repo archive --format=tar -o $archive origin/materials assets/materials
if ($LASTEXITCODE -ne 0) {
    Write-Error "get-materials: the 'materials' branch has no assets/materials"
    exit 1
}

# tar ships with Windows 10 and later.
tar -x -f $archive -C $repo
$extracted = $LASTEXITCODE
Remove-Item $archive -ErrorAction SilentlyContinue
if ($extracted -ne 0) {
    Write-Error "get-materials: could not unpack the library"
    exit 1
}

$count = (Get-ChildItem -Recurse -Path (Join-Path $repo 'assets/materials') -Filter '*.fmat').Count
Write-Host "get-materials: $count material(s) in assets/materials" -ForegroundColor Green
