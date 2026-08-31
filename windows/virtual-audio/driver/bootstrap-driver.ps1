param(
    [string]$Destination = "$PSScriptRoot\AudioMirror-src"
)

$ErrorActionPreference = 'Stop'

$repo = 'https://github.com/JannesP/AudioMirror.git'
$commit = 'a9618d1ab4114e3e35e8e50ae804a4205315d1f2'

if (Test-Path $Destination) {
    Remove-Item -Recurse -Force $Destination
}

git clone $repo $Destination
Push-Location $Destination
try {
    git checkout $commit

    $inf = Join-Path $Destination 'AudioMirror\AudioMirror.inf'
    $content = Get-Content $inf -Raw
    $content = $content.Replace('AudioMirror Virtual Device', 'scrcpy Virtual Audio Device')
    $content = $content.Replace('AudioMirror Wave Input', 'scrcpy Virtual Microphone')
    $content = $content.Replace('AudioMirror Topology Input', 'scrcpy Virtual Microphone Topology')
    $content = $content.Replace('AudioMirror Wave Output', 'scrcpy Virtual Microphone Feed')
    $content = $content.Replace('AudioMirror Topology Output', 'scrcpy Virtual Microphone Feed Topology')
    $content = $content.Replace('AudioMirror Drivers', 'scrcpy Virtual Audio Driver')
    Set-Content -Path $inf -Value $content -Encoding ascii

    # The pinned AudioMirror source predates current Windows 11 WDK headers.
    # Newer kits emit deprecation warnings (for example ExAllocatePoolWithTag)
    # and current driver defaults can promote warnings to errors. The upstream
    # project only overrides TreatWarningAsError for Debug|x64, while CI builds
    # Release|x64, so add an explicit Release compiler definition here.
    $project = Join-Path $Destination 'AudioMirror\AudioMirror.vcxproj'
    $projectContent = Get-Content $project -Raw

    $releaseItemDefinition = @'
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <TreatWarningAsError>false</TreatWarningAsError>
      <DisableSpecificWarnings>4996;4390;%(DisableSpecificWarnings)</DisableSpecificWarnings>
      <PreprocessorDefinitions>_WIN64;_AMD64_;AMD64;%(PreprocessorDefinitions);_NEW_DELETE_OPERATORS_</PreprocessorDefinitions>
    </ClCompile>
    <Link>
      <AdditionalDependencies>$(DDK_LIB_PATH)\portcls.lib;$(DDK_LIB_PATH)\stdunk.lib;$(DDK_LIB_PATH)\libcntpr.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
    <DriverSign>
      <FileDigestAlgorithm>SHA256</FileDigestAlgorithm>
    </DriverSign>
  </ItemDefinitionGroup>
'@

    $marker = '  <ItemGroup>' + [Environment]::NewLine + '    <Inf Include="AudioMirror.inf" />'
    if (-not $projectContent.Contains($marker)) {
        throw 'Could not locate AudioMirror INF item in project.'
    }

    # Insert the Release|x64 overrides immediately before the INF item.
    $projectContent = $projectContent.Replace($marker, $releaseItemDefinition + $marker)

    # The hosted runner currently combines a newer preinstalled SDK with the
    # Chocolatey WDK. Its WDK INF build target tries to load x86\InfVerif.dll,
    # which is absent. Do not run that target during this development compile;
    # keep the patched INF as a normal project file and package it separately.
    $projectContent = $projectContent.Replace(
        '<Inf Include="AudioMirror.inf" />',
        '<None Include="AudioMirror.inf" />')

    Set-Content -Path $project -Value $projectContent -Encoding utf8

    $licenseSource = Join-Path $Destination 'LICENSE.md'
    $licenseDestination = Join-Path $PSScriptRoot 'AUDIOMIRROR-LICENSE.md'
    Copy-Item $licenseSource $licenseDestination -Force
}
finally {
    Pop-Location
}

Write-Host "Prepared pinned AudioMirror driver source at: $Destination"
Write-Host "Open AudioMirror.sln with Visual Studio + WDK and build x64."
Write-Host "The render endpoint will be named 'scrcpy Virtual Microphone Feed'."
Write-Host "The capture endpoint will be named 'scrcpy Virtual Microphone'."
