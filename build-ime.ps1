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
$tsfSources = @('native/tsf_text_store.cc', 'native/tsf_backend.cc') | ForEach-Object { Join-Path $PSScriptRoot $_ }
$output = Join-Path $PSScriptRoot 'neo-ime-native.dll'
& $Compiler -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror -fno-rtti -shared -static "-I$include" -o $output $source @tsfSources -limm32 -lcomctl32 -lole32 -loleaut32 -luuid
if ($LASTEXITCODE -ne 0) { throw 'neo-ime DLL build failed; use a MinGW compiler matching Emacs (x64 on this PC)' }
Write-Output "NEO_IME_BUILD=PASS $output"

# An installable package includes its DLL; package-install-file on .el alone
# would silently omit the native backend.
$packageName = 'neo-ime-0.2.0'
$buildRoot = Join-Path $PSScriptRoot 'var/ime-package'
$packageRoot = Join-Path $buildRoot $packageName
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
foreach ($legacySource in @('native/neo-ime-native.c', 'native/check-ime-native.c')) {
    $legacyPath = Join-Path $packageRoot $legacySource
    if (Test-Path -LiteralPath $legacyPath -PathType Leaf) {
        Remove-Item -LiteralPath $legacyPath
    }
}
foreach ($file in @('neo-ime.el', 'neo-ime-tsf.el', 'neo-ime-native.dll', 'build-ime.ps1')) {
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
foreach ($file in @('tsf_text_store.h', 'tsf_text_store.cc', 'tsf_backend.h', 'tsf_backend.cc', 'tsf_compat.h', 'check_tsf_store.cc', 'check_tsf_input.cc')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "native/$file") -Destination (Join-Path $packageRoot "native/$file")
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '.clang-format') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native/COPYING') -Destination (Join-Path $packageRoot 'native/COPYING')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-performance.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-status.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'benchmark-ime.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-gui.el') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-ime-minibuffer.el') -Destination $packageRoot
foreach ($file in @('check-ime-tsf.el', 'check-ime-tsf-gui.el', 'check-ime-tsf-input.el', 'check-ime-evil.el', 'check-ime-evil-gui.el')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $packageRoot
}
@'
;;; neo-ime-pkg.el --- Package metadata -*- lexical-binding: t; no-byte-compile: t; -*-
(define-package "neo-ime" "0.2.0" "Inline Windows IME composition" '((emacs "29.1")))
'@ | Set-Content -LiteralPath (Join-Path $packageRoot 'neo-ime-pkg.el') -Encoding utf8
$archive = Join-Path $PSScriptRoot 'var/neo-ime-0.2.0.tar'
& tar.exe -cf $archive -C $buildRoot $packageName
if ($LASTEXITCODE -ne 0) { throw 'neo-ime package archive failed' }
Write-Output "NEO_IME_PACKAGE=PASS $archive"
