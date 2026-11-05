#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [Parameter(Mandatory)][string]$StatusPath,
    [string]$ExistingPython = '',
    [string]$ExistingModel = '',
    [switch]$NoRun
)
Set-StrictMode -Version 2.0

function Write-AsrAtomicText {
    param([string]$Path, [string]$Text)
    $Path = [IO.Path]::GetFullPath($Path)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.Encoding]::Unicode)
        for ($attempt = 0; ; $attempt++) {
            try {
                if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
                else { [IO.File]::Move($temporary, $Path) }
                break
            } catch [IO.IOException] {
                if ($attempt -ge 4) { throw }
                Start-Sleep -Milliseconds 100
            }
        }
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}
function Write-AsrStatus {
    param([string]$Path, [ValidateSet('running','ready','error')][string]$State, [string]$Message)
    $Message = $Message -replace '[\r\n\x00]', ' '
    Write-AsrAtomicText $Path "[setup]`r`nstate=$State`r`nmessage=$Message`r`n"
}
function Test-AsrHash {
    param([string]$Path, [string]$Sha256)
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Sha256
}
# All installation side effects are kept behind Invoke-AsrSetup for offline dot-source tests.
function Get-AsrManifest {
    $path = Join-Path $PSScriptRoot 'asr_downloads.json'
    $manifest = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($manifest.schema -ne 1) { throw '下载清单版本不受支持' }
    return $manifest
}

function Get-AsrSources {
    param($Artifact)
    # Order is intentional: domestic mirror before upstream, without mutable revisions.
    return @($Artifact.urls)
}

function Test-AsrArtifact {
    param([string]$Path, $Artifact)
    if (!(Test-AsrHash $Path $Artifact.sha256)) { return $false }
    if ($Artifact.PSObject.Properties['size'] -and $Artifact.size -gt 0) {
        if ((Get-Item -LiteralPath $Path).Length -ne $Artifact.size) { return $false }
    }
    # Git SHA-1 is an ADDITIONAL check against HF's official tree for non-LFS metadata.
    if ($Artifact.PSObject.Properties['gitBlob'] -and $Artifact.gitBlob) {
        $bytes = [IO.File]::ReadAllBytes($Path)
        $header = [Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)" + [char]0)
        $hash = [Security.Cryptography.SHA1]::Create()
        try {
            $all = New-Object byte[] ($header.Length + $bytes.Length)
            [Array]::Copy($header, 0, $all, 0, $header.Length)
            [Array]::Copy($bytes, 0, $all, $header.Length, $bytes.Length)
            $actual = [BitConverter]::ToString($hash.ComputeHash($all)).Replace('-', '').ToLowerInvariant()
            if ($actual -ne $Artifact.gitBlob) { return $false }
        } finally { $hash.Dispose() }
    }
    return $true
}

function Receive-AsrHttpFile {
    param([string]$Url, [string]$Destination, [string]$StatusPath,
          [string]$Label, [int]$TimeoutSeconds = 300)
    if (([Uri]$Url).Scheme -ne 'https') { throw '下载地址必须使用 HTTPS' }
    $request = [Net.HttpWebRequest]::Create($Url)
    $request.UserAgent = 'GlobalRecorder-ASR-Setup/1'
    $request.Timeout = 30000
    $request.ReadWriteTimeout = 15000
    $request.MaximumAutomaticRedirections = 6
    $response = $null
    $inputStream = $null
    $outputStream = $null
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        $response = $request.GetResponse()
        if ($response.ResponseUri.Scheme -ne 'https') { throw '拒绝非 HTTPS 重定向' }
        $inputStream = $response.GetResponseStream()
        $outputStream = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $buffer = New-Object byte[] 65536
        [long]$received = 0
        [long]$nextUpdate = 0
        while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            if ($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw '下载超时' }
            $outputStream.Write($buffer, 0, $count)
            $received += $count
            if ($clock.ElapsedMilliseconds -ge $nextUpdate) {
                $detail = if ($response.ContentLength -gt 0) {
                    '{0}%' -f [Math]::Min(100, [Math]::Floor(100.0 * $received / $response.ContentLength))
                } else { '{0:N1} MB' -f ($received / 1MB) }
                Write-AsrStatus $StatusPath running ("{0}：{1}" -f $Label, $detail)
                $nextUpdate = $clock.ElapsedMilliseconds + 1000
            }
        }
        $outputStream.Flush($true)
        if ($response.ContentLength -ge 0 -and $received -ne $response.ContentLength) {
            throw '下载文件不完整'
        }
    } finally {
        if ($outputStream) { $outputStream.Dispose() }
        if ($inputStream) { $inputStream.Dispose() }
        if ($response) { $response.Dispose() }
        $request.Abort()
    }
}

function Get-AsrArtifact {
    param($Artifact, [string]$CacheDirectory, [string]$StatusPath, [string]$Label)
    if ($Artifact.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw '下载清单缺少固定 SHA256' }
    [IO.Directory]::CreateDirectory($CacheDirectory) | Out-Null
    $destination = Join-Path $CacheDirectory ($Artifact.sha256 + '.cache')
    if (Test-AsrArtifact $destination $Artifact) {
        Write-AsrStatus $StatusPath running ("复用已校验文件：{0}" -f $Artifact.name)
        return $destination
    }
    foreach ($url in (Get-AsrSources $Artifact)) {
        for ($attempt = 1; $attempt -le 2; $attempt++) {
            $temporary = Join-Path $CacheDirectory ('.part-' + [Guid]::NewGuid().ToString('N'))
            try {
                Write-AsrStatus $StatusPath running ("{0}（尝试 {1}/2）" -f $Label, $attempt)
                Receive-AsrHttpFile $url $temporary $StatusPath $Label
                if (!(Test-AsrArtifact $temporary $Artifact)) { throw '下载文件校验失败' }
                # Only validated bytes replace the cache; a failed transfer never removes old bytes.
                if ([IO.File]::Exists($destination)) { [IO.File]::Replace($temporary, $destination, [NullString]::Value) }
                else { [IO.File]::Move($temporary, $destination) }
                return $destination
            } catch {
                Write-Verbose ("下载来源失败：{0}" -f $_.Exception.Message)
                if ($attempt -eq 2) {
                    Write-AsrStatus $StatusPath running ("当前来源失败，切换下载源：{0}" -f $Artifact.name)
                }
            } finally {
                if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
            }
        }
    }
    throw ("下载或校验失败：{0}" -f $Artifact.name)
}

function ConvertTo-AsrProcessArgument {
    param([AllowEmptyString()][string]$Value)
    # Windows CommandLineToArgvW / Python CRT escaping, including trailing backslashes.
    return '"' + [regex]::Replace(
        [regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Invoke-AsrProcess {
    param([string]$Python, [string[]]$Arguments, [int]$TimeoutSeconds = 120,
          [string]$StatusPath = '', [string]$Label = '检查转写组件')
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Python
    $start.Arguments = (($Arguments | ForEach-Object { ConvertTo-AsrProcessArgument $_ }) -join ' ')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # Child-only configuration; existing Python, global PATH and registry are never written.
    foreach ($name in @($start.EnvironmentVariables.Keys)) {
        if ($name -match '^(PYTHON|PIP_)') { $start.EnvironmentVariables.Remove($name) }
    }
    $start.EnvironmentVariables['PYTHONDONTWRITEBYTECODE'] = '1'
    $start.EnvironmentVariables['PYTHONNOUSERSITE'] = '1'
    $start.EnvironmentVariables['PIP_CONFIG_FILE'] = 'NUL'
    $start.EnvironmentVariables['PIP_NO_INPUT'] = '1'
    $start.EnvironmentVariables['HF_HUB_OFFLINE'] = '1'
    $start.EnvironmentVariables['TRANSFORMERS_OFFLINE'] = '1'
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    $started = $false
    try {
        if (!$process.Start()) { throw '无法启动本地运行组件' }
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while (!$process.WaitForExit(1000)) {
            if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                $process.Kill()
                $process.WaitForExit()
                throw '运行组件操作超时'
            }
            if ($StatusPath -and ([int]$clock.Elapsed.TotalSeconds % 3 -eq 0)) {
                Write-AsrStatus $StatusPath running $Label
            }
        }
        $process.WaitForExit()
        # Drain both pipes concurrently to avoid pip output deadlocks. Do not expose its logs.
        $null = $stdout.GetAwaiter().GetResult()
        $null = $stderr.GetAwaiter().GetResult()
        return $process.ExitCode
    } finally {
        if ($started -and !$process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
        $process.Dispose()
    }
}

function Test-AsrPython {
    param([string]$Python)
    if (!$Python -or !(Test-Path -LiteralPath $Python -PathType Leaf)) { return $false }
    if ([IO.Path]::GetFileName($Python) -notmatch '^pythonw?\.exe$') { return $false }
    try {
        $result = Invoke-AsrProcess $Python @('-I', '-B', '-c',
            'import faster_whisper, ctranslate2, av; assert callable(faster_whisper.WhisperModel)')
        return $result -eq 0
    } catch { return $false }
}

function Test-AsrModelFiles {
    param([string]$Directory, $Artifacts = $null)
    if (!$Directory -or !(Test-Path -LiteralPath $Directory -PathType Container)) { return $false }
    foreach ($name in @('config.json','model.bin','tokenizer.json')) {
        $path = Join-Path $Directory $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
            return $false
        }
    }
    $hasVocabulary = $false
    foreach ($name in @('vocabulary.txt','vocabulary.json')) {
        $path = Join-Path $Directory $name
        if ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-Item -LiteralPath $path).Length -gt 0) {
            $hasVocabulary = $true
        }
    }
    if (!$hasVocabulary) { return $false }
    try {
        $jsonNames = @('config.json','tokenizer.json')
        foreach ($optional in @('preprocessor_config.json','vocabulary.json')) {
            if (Test-Path -LiteralPath (Join-Path $Directory $optional) -PathType Leaf) {
                $jsonNames += $optional
            }
        }
        foreach ($name in $jsonNames) {
            $parsed = [IO.File]::ReadAllText((Join-Path $Directory $name), [Text.Encoding]::UTF8) | ConvertFrom-Json
            if ($null -eq $parsed) { return $false }
        }
        if ($Artifacts) {
            foreach ($artifact in $Artifacts) {
                if (!(Test-AsrArtifact (Join-Path $Directory $artifact.name) $artifact)) { return $false }
            }
        }
        return $true
    } catch { return $false }
}

function Test-AsrModelLoad {
    param([string]$Python, [string]$Directory, [string]$StatusPath)
    if (!(Test-AsrModelFiles $Directory)) { return $false }
    # No audio, no server, and no hidden HF fallback: the final test actually loads local weights.
    $code = 'import sys; from faster_whisper import WhisperModel; m=WhisperModel(sys.argv[1], device="cpu", compute_type="int8", cpu_threads=2, num_workers=1, local_files_only=True)'
    try {
        return (Invoke-AsrProcess $Python @('-I', '-B', '-c', $code, $Directory) 180 $StatusPath '验证本地模型') -eq 0
    } catch { return $false }
}

function Expand-AsrZip {
    param([string]$Archive, [string]$Directory)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Directory]::CreateDirectory($Directory) | Out-Null
    # Both archives have fixed upstream SHA256 and are extracted only inside a fresh stage.
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Directory)
}

function Assert-AsrInstallBase {
    param([string]$ProjectRoot, [string]$BaseDirectory)
    if (!$ProjectRoot) { throw '安装目录缺少项目根路径' }
    $root = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\')
    $expected = [IO.Path]::GetFullPath((Join-Path $root 'runtime\asr')).TrimEnd('\')
    $actual = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd('\')
    if (!$actual.Equals($expected, [StringComparison]::OrdinalIgnoreCase)) {
        throw '安装目录超出当前项目 runtime/asr'
    }
    foreach ($path in @($root, (Join-Path $root 'runtime'), $actual)) {
        if ((Test-Path -LiteralPath $path) -and
            ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw '安装目录不能是符号链接或目录联接'
        }
    }
}

function Move-AsrStage {
    param([string]$ProjectRoot, [string]$BaseDirectory, [string]$Stage, [string]$Destination)
    Assert-AsrInstallBase $ProjectRoot $BaseDirectory
    $base = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd('\')
    $source = [IO.Path]::GetFullPath($Stage)
    $target = [IO.Path]::GetFullPath($Destination)
    if (![IO.Path]::GetDirectoryName($source).Equals($base, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($source) -notmatch '^\.(python|model)-[a-f0-9]{32}$' -or
        ![IO.Path]::GetDirectoryName($target).Equals($base, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($target) -notmatch '^(python-|tiny-)[a-zA-Z0-9.-]+$') {
        throw '拒绝移动项目外路径或非本次 GUID 暂存目录'
    }
    if ((Get-Item -LiteralPath $source -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw '拒绝移动符号链接暂存目录'
    }
    if (Test-Path -LiteralPath $target) { throw '拒绝覆盖已有运行组件或模型目录' }
    [IO.Directory]::Move($source, $target)
}

function Install-AsrPython {
    param($Manifest, [string]$BaseDirectory, [string]$StatusPath, [string]$ProjectRoot)
    Assert-AsrInstallBase $ProjectRoot $BaseDirectory
    $final = Join-Path $BaseDirectory 'python-3.11.9-fw-1.2.1-v1'
    $python = Join-Path $final 'python.exe'
    if (Test-AsrPython $python) {
        Write-AsrStatus $StatusPath running '复用本地转写组件'
        return $python
    }
    $stage = Join-Path $BaseDirectory ('.python-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    $cache = Join-Path $BaseDirectory 'downloads'
    try {
        $archive = Get-AsrArtifact $Manifest.python $cache $StatusPath '下载 Python 运行组件'
        Expand-AsrZip $archive $stage
        $site = Join-Path $stage 'Lib\site-packages'
        [IO.Directory]::CreateDirectory($site) | Out-Null
        [IO.File]::WriteAllText((Join-Path $stage 'python311._pth'),
            "python311.zip`r`n.`r`nLib\site-packages`r`nimport site`r`n", [Text.Encoding]::ASCII)
        $pip = Get-AsrArtifact $Manifest.pip $cache $StatusPath '下载 pip 引导组件'
        Expand-AsrZip $pip $site
        $wheel = Get-AsrArtifact $Manifest.fasterWhisper $cache $StatusPath '下载转写组件'
        # pip requires the original .whl basename, rather than our content-addressed cache name.
        $wheelPath = Join-Path $stage $Manifest.fasterWhisper.name
        [IO.File]::Copy($wheel, $wheelPath, $true)
        $stagePython = Join-Path $stage 'python.exe'
        $installed = $false
        foreach ($index in $Manifest.pipIndexes) {
            Write-AsrStatus $StatusPath running '安装转写组件及依赖'
            $arguments = @('-I','-B','-m','pip','--isolated','--disable-pip-version-check',
                'install','--no-input','--no-cache-dir','--only-binary=:all:',
                '--timeout','30','--retries','1','--index-url',$index,
                '--target',$site,'--upgrade',$wheelPath)
            if ((Invoke-AsrProcess $stagePython $arguments 900 $StatusPath '安装转写组件及依赖') -eq 0) {
                $installed = $true
                break
            }
        }
        if (!$installed -or !(Test-AsrPython $stagePython)) {
            throw '转写组件安装失败或缺少系统运行库'
        }
        [IO.File]::Delete($wheelPath)
        # Immutable publication: even a broken previous version is retained, never recursively deleted.
        if ([IO.Directory]::Exists($final)) {
            $final += '-' + [Guid]::NewGuid().ToString('N')
        }
        Move-AsrStage $ProjectRoot $BaseDirectory $stage $final
        return (Join-Path $final 'python.exe')
    } finally {
        # Staging files deliberately survive failures for inspection; never touch a prior runtime.
    }
}

function Install-AsrModel {
    param($Manifest, [string]$BaseDirectory, [string]$StatusPath, [string]$ProjectRoot)
    Assert-AsrInstallBase $ProjectRoot $BaseDirectory
    $final = Join-Path $BaseDirectory ('tiny-' + $Manifest.modelRevision + '-v1')
    if (Test-AsrModelFiles $final $Manifest.modelFiles) {
        Write-AsrStatus $StatusPath running '复用已校验的 tiny 模型'
        return $final
    }
    $stage = Join-Path $BaseDirectory ('.model-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    $cache = Join-Path $BaseDirectory 'downloads'
    $number = 0
    foreach ($artifact in $Manifest.modelFiles) {
        $number++
        $label = '下载模型 {0}/{1}' -f $number, $Manifest.modelFiles.Count
        $file = Get-AsrArtifact $artifact $cache $StatusPath $label
        [IO.File]::Copy($file, (Join-Path $stage $artifact.name), $true)
    }
    if (!(Test-AsrModelFiles $stage $Manifest.modelFiles)) { throw '模型完整性校验失败' }
    if ([IO.Directory]::Exists($final)) { $final += '-' + [Guid]::NewGuid().ToString('N') }
    Move-AsrStage $ProjectRoot $BaseDirectory $stage $final
    return $final
}

function Get-AsrReadyValues {
    param([string]$Path)
    $values = @{}
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $values }
    $section = ''
    foreach ($line in [IO.File]::ReadAllLines($Path, [Text.Encoding]::Unicode)) {
        if ($line -match '^\[(.+)\]$') { $section = $Matches[1]; continue }
        if ($section -eq 'asr' -and $line -match '^(python|model)=(.+)$') {
            $values[$Matches[1]] = $Matches[2]
        }
    }
    return $values
}

function Invoke-AsrSetup {
    param([string]$ProjectRoot, [string]$StatusPath,
          [string]$ExistingPython = '', [string]$ExistingModel = '')
    $lock = $null
    $protocol = [Net.ServicePointManager]::SecurityProtocol
    $stage = '准备本地转写'
    $canWriteStatus = $true
    try {
        $ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
        $StatusPath = [IO.Path]::GetFullPath($StatusPath)
        $readyPath = Join-Path $ProjectRoot 'runtime\asr-ready.ini'
        if ($StatusPath -eq $readyPath) {
            $canWriteStatus = $false
            throw '进度文件不能与就绪文件相同'
        }
        if (($ProjectRoot + $StatusPath + $ExistingPython + $ExistingModel) -match '[\r\n\x00]') {
            throw '路径包含无效字符'
        }
        Write-AsrStatus $StatusPath running $stage
        if (![Environment]::Is64BitOperatingSystem) { throw '转写组件需要 64 位 Windows' }
        $base = Join-Path $ProjectRoot 'runtime\asr'
        Assert-AsrInstallBase $ProjectRoot $base
        [IO.Directory]::CreateDirectory($base) | Out-Null
        try {
            $lock = [IO.File]::Open((Join-Path $base 'install.lock'),
                [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        } catch { throw '另一个转写安装任务正在运行' }
        [Net.ServicePointManager]::SecurityProtocol = $protocol -bor [Net.SecurityProtocolType]::Tls12
        $manifest = Get-AsrManifest
        $old = Get-AsrReadyValues $readyPath
        $stage = '检查转写运行组件'
        Write-AsrStatus $StatusPath running $stage
        $python = ''
        foreach ($candidate in @($ExistingPython, $old['python'])) {
            if ($candidate -and (Test-AsrPython $candidate)) {
                $python = [IO.Path]::GetFullPath($candidate)
                break
            }
        }
        if (!$python) { $python = Install-AsrPython $manifest $base $StatusPath $ProjectRoot }
        $stage = '检查本地模型'
        Write-AsrStatus $StatusPath running $stage
        $model = ''
        foreach ($candidate in @($ExistingModel, $old['model'])) {
            if ($candidate -and (Test-AsrModelLoad $python $candidate $StatusPath)) {
                $model = [IO.Path]::GetFullPath($candidate)
                break
            }
        }
        if (!$model) {
            $model = Install-AsrModel $manifest $base $StatusPath $ProjectRoot
            if (!(Test-AsrModelLoad $python $model $StatusPath)) { throw '本地模型加载失败' }
        }
        $stage = '发布本地转写配置'
        Write-AsrStatus $StatusPath running $stage
        Write-AsrAtomicText $readyPath "[asr]`r`npython=$python`r`nmodel=$model`r`n"
        Write-AsrStatus $StatusPath ready '本地转写已就绪'
        return 0
    } catch {
        $message = '安装失败：' + $_.Exception.Message
        if ($message.Length -gt 160) { $message = '安装失败：' + $stage + '，请检查网络或目录权限后重试' }
        if ($canWriteStatus) { try { Write-AsrStatus $StatusPath error $message } catch { } }
        return 1
    } finally {
        [Net.ServicePointManager]::SecurityProtocol = $protocol
        if ($lock) { $lock.Dispose() }
    }
}

if (!$NoRun) {
    $ErrorActionPreference = 'Stop'
    exit (Invoke-AsrSetup $ProjectRoot $StatusPath $ExistingPython $ExistingModel)
}
