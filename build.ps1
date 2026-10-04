param(
    [string]$Compiler = '',
    [switch]$Test,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

if (-not $Compiler) {
    $compilerCommand = Get-Command 'g++' -CommandType Application -ErrorAction SilentlyContinue
    if (-not $compilerCommand) { throw 'PATH 中找不到 g++，请添加 MinGW-w64 工具目录或使用 -Compiler 指定完整路径。' }
    $Compiler = $compilerCommand.Source
} else {
    $Compiler = (Resolve-Path -LiteralPath $Compiler).Path
}
$compilerDirectory = Split-Path $Compiler -Parent

$previousPath = $env:PATH
try {
    $env:PATH = "$compilerDirectory;$previousPath"
    $resourceCommand = Get-Command 'windres' -CommandType Application -ErrorAction SilentlyContinue
    if (-not $resourceCommand) { throw 'PATH 中找不到 windres，请添加 MinGW-w64 工具目录。' }
    $resourceCompiler = $resourceCommand.Source
    Push-Location $projectRoot
    try {
        & $resourceCompiler '-I' 'src' 'src/countdown.rc' '-O' 'coff' '-o' 'build/resources.o'
        if ($LASTEXITCODE -ne 0) { throw '资源编译失败。' }
        & $Compiler '-std=c++17' '-Os' '-flto' '-fno-exceptions' '-fno-rtti' '-ffunction-sections' '-fdata-sections' '-Wall' '-Wextra' '-Wpedantic' '-Wno-missing-field-initializers' '-municode' '-mwindows' '-static-libgcc' '-static-libstdc++' 'src/main.cpp' 'build/resources.o' '-Wl,--gc-sections,--strip-all' '-luser32' '-lgdi32' '-lcomctl32' '-lcomdlg32' '-lshell32' '-lole32' '-lsapi' '-luuid' '-o' 'build/countdown.exe'
        if ($LASTEXITCODE -ne 0) { throw 'C++ 编译失败。' }
        if ($Test) {
            & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-static-libgcc' '-static-libstdc++' '-Isrc' 'tests/timer_test.cpp' '-o' 'build/timer_test.exe'
            if ($LASTEXITCODE -ne 0) { throw '测试编译失败。' }
            & '.\build\timer_test.exe'
            if ($LASTEXITCODE -ne 0) { throw '计时器测试失败。' }
        }
        $deliverable = Join-Path $outputDirectory 'countdown.exe'
        if (-not $BuildOnly) {
            $releaseDirectory = Join-Path $projectRoot 'dist'
            New-Item -ItemType Directory -Force -Path $releaseDirectory | Out-Null
            $deliverable = Join-Path $releaseDirectory 'countdown.exe'
            Copy-Item -LiteralPath (Join-Path $outputDirectory 'countdown.exe') -Destination $deliverable -Force
            Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $releaseDirectory -Force
        }
        $binary = Get-Item -LiteralPath $deliverable
        Write-Output "已生成：$($binary.FullName) ($($binary.Length) 字节)"
    } finally { Pop-Location }
} finally { $env:PATH = $previousPath }
