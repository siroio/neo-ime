param(
    [string]$EmacsRoot,
    [string]$Compiler = 'g++'
)
$ErrorActionPreference = 'Stop'
if (-not $EmacsRoot) {
    # Ask the running Emacs, so Scoop shims do not determine the include path.
    $EmacsRoot = (& emacs.exe -Q --batch --eval '(princ (expand-file-name ".." invocation-directory))' | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot locate Emacs runtime' }
}
$header = Join-Path $EmacsRoot 'include/emacs-module.h'
if (-not (Test-Path -LiteralPath $header)) { throw "Missing module header: $header" }
$include = Split-Path -Parent $header
$source = Join-Path $PSScriptRoot 'native/neo_ime_native.cc'
$output = Join-Path $PSScriptRoot 'neo-ime-native.dll'
& $Compiler -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror -fno-exceptions -fno-rtti -shared -static "-I$include" -o $output $source -limm32 -lcomctl32
if ($LASTEXITCODE -ne 0) { throw 'neo-ime DLL build failed; use a MinGW compiler matching Emacs (x64 on this PC)' }
Write-Output "NEO_IME_BUILD=PASS $output"

# An installable package includes its DLL; package-install-file on .el alone
# would silently omit the native backend.
$packageName = 'neo-ime-0.1.4'
$buildRoot = Join-Path $PSScriptRoot 'var/ime-package'
$packageRoot = Join-Path $buildRoot $packageName
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
foreach ($legacySource in @('native/neo-ime-native.c', 'native/check-ime-native.c')) {
    $legacyPath = Join-Path $packageRoot $legacySource
    if (Test-Path -LiteralPath $legacyPath -PathType Leaf) {
        Remove-Item -LiteralPath $legacyPath
    }
}
foreach ($file in @('neo-ime.el', 'neo-ime-native.dll', 'build-ime.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $packageRoot
}
$imeReadme = Join-Path $PSScriptRoot 'docs/neo-ime.md'
if (-not (Test-Path -LiteralPath $imeReadme)) { $imeReadme = Join-Path $PSScriptRoot 'README.md' }
Copy-Item -LiteralPath $imeReadme -Destination (Join-Path $packageRoot 'README.md')
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE') -Destination $packageRoot
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native/COPYING') -Destination (Join-Path $packageRoot 'COPYING')
New-Item -ItemType Directory -Force -Path (Join-Path $packageRoot 'native') | Out-Null
Copy-Item -LiteralPath $source -Destination (Join-Path $packageRoot 'native/neo_ime_native.cc')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native/neo_ime_native.h') -Destination (Join-Path $packageRoot 'native/neo_ime_native.h')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native/check_ime_native.cc') -Destination (Join-Path $packageRoot 'native/check_ime_native.cc')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '.clang-format') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native/COPYING') -Destination (Join-Path $packageRoot 'native/COPYING')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-performance.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-status.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'benchmark-ime.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-gui.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-minibuffer.el') -Destination $packageRoot
@'
;;; neo-ime-pkg.el --- Package metadata -*- lexical-binding: t; no-byte-compile: t; -*-
(define-package "neo-ime" "0.1.4" "Inline Windows IME composition" '((emacs "29.1")))
'@ | Set-Content -LiteralPath (Join-Path $packageRoot 'neo-ime-pkg.el') -Encoding utf8
$archive = Join-Path $PSScriptRoot 'var/neo-ime-0.1.4.tar'
& tar.exe -cf $archive -C $buildRoot $packageName
if ($LASTEXITCODE -ne 0) { throw 'neo-ime package archive failed' }
Write-Output "NEO_IME_PACKAGE=PASS $archive"
