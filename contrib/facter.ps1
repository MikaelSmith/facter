### Set variables from command line
# $arch => Choose 32 or 64-bit build
# $cores => Set the number of cores to use for parallel builds
# $buildSource => Choose whether to download pre-built libraries or build from source
param (
[int] $arch=64,
[int] $cores=2,
[bool] $buildSource=$FALSE,
[string] $facterRef='origin/master',
[string] $facterFork='git://github.com/puppetlabs/facter'
)

$ErrorActionPreference = 'Stop'

# Ensure TEMP directory is set and exists. Git.install can fail otherwise.
try {
    if (!(Test-Path $env:TEMP)) { throw }
} catch {
    $env:TEMP = Join-Path $env:SystemDrive 'temp'
    echo "TEMP not correct, setting to $env:TEMP"
}
if (!(Test-Path $env:TEMP)) {
    mkdir -Path $env:TEMP
    echo "TEMP dir $env:TEMP created"
}

if ($env:Path -eq $null) {
    echo "Path is null?"
}

# Starting from a base Windows Server 2008r2 or 2012r2 installation, install required tools, setup the PATH, and download and build software.
# This script can be run directly from the web using "iex ((new-object net.webclient).DownloadString('<url_to_raw>'))"

### Configuration
## Setup the working directory
$sourceDir=$pwd

echo $arch
echo $cores
echo $buildSource


$mingwVerNum = "4.8.3"
$mingwVerChoco = $mingwVerNum
$mingwThreads = "win32"
if ($arch -eq 64) {
  $mingwExceptions = "seh"
  $mingwArch = "x86_64"
} else {
  $mingwExceptions = "sjlj"
  $mingwArch = "i686"
}
$mingwVer = "${mingwArch}_mingw-w64_${mingwVerNum}_${mingwThreads}_${mingwExceptions}"

$boostVer = "boost_1_58_0"
$boostPkg = "${boostVer}-${mingwVer}"

$yamlCppVer = "yaml-cpp-0.5.1"
$yamlPkg = "${yamlCppVer}-${mingwVer}"

$curlVer = "curl-7.42.1"
$curlPkg = "${curlVer}-${mingwVer}"

### Setup, build, and install
## Install Chocolatey, then use it to install required tools.
Function Install-Choco ($pkg, $ver, $opts = "") {
    echo "Installing $pkg $ver from https://www.myget.org/F/puppetlabs"
    try {
        choco install -y $pkg -version $ver -source https://www.myget.org/F/puppetlabs -debug $opts
    } catch {
        echo "Error: $_, trying again."
        choco install -y $pkg -version $ver -source https://www.myget.org/F/puppetlabs -debug $opts
    }
}

if (!(Get-Command choco -ErrorAction SilentlyContinue)) {
    iex ((new-object net.webclient).DownloadString('https://chocolatey.org/install.ps1'))
}
Install-Choco 7zip.commandline 9.20.0.20150210
Install-Choco cmake 3.2.2
Install-Choco git.install 2.6.2.20151028

# For MinGW, we expect specific project defaults
# - win32 threads, for Windows Server 2003 support
# - seh exceptions on 64-bit, to work around an obscure bug loading Ruby in Facter
# These are the defaults on our myget feed.
if ($arch -eq 64) {
  Install-Choco ruby 2.1.6
  Install-Choco mingw-w64 $mingwVerChoco
} else {
  Install-Choco ruby 2.1.6 @('-x86')
  Install-Choco mingw-w32 $mingwVerChoco @('-x86')
}
$env:PATH = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
if ($arch -eq 32) {
  $env:PATH = "C:\tools\mingw32\bin;" + $env:PATH
}
$env:PATH += [Environment]::GetFolderPath('ProgramFilesX86') + "\Git\cmd"
echo $env:PATH
cd $sourceDir

function Invoke-External
{
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)]
    [ScriptBlock]
    $cmd
  )

  $Global:LASTEXITCODE = 0
  & $cmd
  if ($LASTEXITCODE -ne 0) { throw ("Terminating.  Last command failed with exit code $LASTEXITCODE") }
}

## Download facter and setup build directories
Invoke-External { git clone $facterFork facter }
cd facter
Invoke-External { git checkout $facterRef }
Invoke-External { git submodule update --init --recursive }
mkdir -Force release
cd release
$buildDir=$pwd
$toolsDir="${sourceDir}\deps"
mkdir -Force $toolsDir
cd $toolsDir

if ($buildSource) {
  ## Download, build, and install Boost
  (New-Object net.webclient).DownloadFile("http://downloads.sourceforge.net/boost/$boostVer.7z", "$toolsDir\$boostVer.7z")
  Invoke-External { & 7za x "${boostVer}.7z" | FIND /V "ing " }
  cd $boostVer

  Invoke-External { .\bootstrap mingw }
  $boost_args = @(
    'toolset=gcc',
    "--build-type=minimal",
    "install",
    '--with-atomic',
    "--with-chrono",
    '--with-container',
    "--with-date_time",
    '--with-exception',
    "--with-filesystem",
    '--with-graph',
    '--with-graph_parallel',
    '--with-iostreams',
    "--with-locale",
    "--with-log",
    '--with-math',
    "--with-program_options",
    "--with-random",
    "--with-regex",
    '--with-serialization',
    '--with-signals',
    "--with-system",
    '--with-test',
    "--with-thread",
    '--with-timer',
    '--with-wave',
    "--prefix=`"$toolsDir\$boostPkg`"",
    "boost.locale.iconv=off"
    "-j$cores"
  )
  Invoke-External { .\b2 $boost_args }
  cd $toolsDir

  ## Download, build, and install yaml-cpp
  (New-Object net.webclient).DownloadFile("https://yaml-cpp.googlecode.com/files/${yamlCppVer}.tar.gz", "$toolsDir\${yamlCppVer}.tar.gz")
  Invoke-External { & 7za x "${yamlCppVer}.tar.gz" }
  Invoke-External { & 7za x "${yamlCppVer}.tar" | FIND /V "ing " }
  cd $yamlCppVer
  mkdir build
  cd build

  $yamlcpp_args = @(
    '-G',
    "MinGW Makefiles",
    "-DBOOST_ROOT=`"$toolsDir\$boostPkg`"",
    "-DCMAKE_INSTALL_PREFIX=`"$toolsDir\$yamlPkg`"",
    ".."
  )
  Invoke-External { cmake $yamlcpp_args }
  Invoke-External { mingw32-make install -j $cores }
  cd $toolsDir

  (New-Object net.webclient).DownloadFile("http://curl.haxx.se/download/${curlVer}.zip", "$toolsDir\${curlVer}.zip")
  Invoke-External { & 7za x "${curlVer}.zip" | FIND /V "ing " }
  cd $curlVer

  Invoke-External { mingw32-make mingw32 }
  mkdir -Path $toolsDir\$curlPkg\include
  cp -r include\curl $toolsDir\$curlPkg\include
  mkdir -Path $toolsDir\$curlPkg\lib
  cp lib\libcurl.a $toolsDir\$curlPkg\lib
  cd $toolsDir
} else {
  ## Download and unpack Boost from a pre-built package in S3
  (New-Object net.webclient).DownloadFile("https://s3.amazonaws.com/kylo-pl-bucket/${boostPkg}.7z", "$toolsDir\${boostPkg}.7z")
  Invoke-External { & 7za x "${boostPkg}.7z" | FIND /V "ing " }

  ## Download and unpack yaml-cpp from a pre-built package in S3
  (New-Object net.webclient).DownloadFile("https://s3.amazonaws.com/kylo-pl-bucket/${yamlPkg}.7z", "$toolsDir\${yamlPkg}.7z")
  Invoke-External { & 7za x "${yamlPkg}.7z" | FIND /V "ing " }

  ## Download and unpack curl from a pre-built package in S3
  (New-Object net.webclient).DownloadFile("https://s3.amazonaws.com/kylo-pl-bucket/${curlPkg}.7z", "$toolsDir\${curlPkg}.7z")
  Invoke-External { & 7za x "${curlPkg}.7z" | FIND /V "ing " }
}

## Build Facter
cd $buildDir
$facter_args = @(
  '-G',
  "MinGW Makefiles",
  "-DBOOST_ROOT=`"$toolsDir\$boostPkg`"",
  "-DBOOST_STATIC=ON",
  "-DYAMLCPP_ROOT=`"$toolsDir\$yamlPkg`"",
  "-DCMAKE_PREFIX_PATH=`"$toolsDir\$curlPkg`"",
  "-DCURL_STATIC=ON",
  ".."
)
Invoke-External { cmake $facter_args }
Invoke-External { mingw32-make -j $cores }

## Write out the version that was just built.
Invoke-External { git describe --long | Out-File -FilePath 'bin/VERSION' -Encoding ASCII -Force }

## Test the results.
Invoke-External { mingw32-make test ARGS=-V }
