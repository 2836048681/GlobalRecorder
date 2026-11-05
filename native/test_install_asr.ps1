#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$script:passed = 0
$script:failed = 0
$script:assertions = 0
$script:testProjectRoot = Join-Path ([IO.Path]::GetTempPath()) ('gr-asr-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
$script:testRoot = Join-Path $script:testProjectRoot ('runtime\asr\.test-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($script:testRoot) | Out-Null
$installer = Join-Path $PSScriptRoot 'install_asr.ps1'
. $installer -ProjectRoot $script:testRoot -StatusPath (Join-Path $script:testRoot 'unused.ini') -NoRun
function Assert-Asr {
    param([bool]$Condition, [string]$Message)
    $script:assertions++
    if (!$Condition) { throw $Message }
}
function Test-Case {
    param([string]$Name, [scriptblock]$Body)
    try { & $Body; $script:passed++; Write-Host "PASS $Name" }
    catch { $script:failed++; Write-Host ("FAIL {0}: {1}" -f $Name, $_.Exception.Message) }
}
function New-TestDirectory {
    $dir = Join-Path $script:testRoot ([Guid]::NewGuid().ToString('N').Substring(0,8) + ' 中文 路径')
    [IO.Directory]::CreateDirectory($dir) | Out-Null
    return $dir
}
function New-TestArtifact {
    param([string]$Source, [string]$Name = 'fixture.bin')
    return [pscustomobject]@{
        name=$Name; size=(Get-Item -LiteralPath $Source).Length
        sha256=(Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLowerInvariant()
        urls=@("https://hf-mirror.com/test/$Name", "https://huggingface.co/test/$Name")
    }
}
function New-TestModel {
    param([string]$Directory)
    [IO.Directory]::CreateDirectory($Directory) | Out-Null
    $artifacts = @()
    foreach ($name in @('config.json','model.bin','tokenizer.json','vocabulary.txt','preprocessor_config.json')) {
        $data = if ($name.EndsWith('.json')) { '{}' } else { 'fixture ' + $name }
        $path = Join-Path $Directory $name
        [IO.File]::WriteAllText($path, $data, [Text.UTF8Encoding]::new($false))
        $artifacts += New-TestArtifact $path $name
    }
    return [pscustomobject]@{ modelRevision='offline-fixture'; modelFiles=$artifacts }
}
function New-TestRuntime {
    param([string]$Directory)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Directory]::CreateDirectory($Directory)|Out-Null
    $pythonFiles=Join-Path $Directory 'python-files'
    $pipFiles=Join-Path $Directory 'pip-files'
    [IO.Directory]::CreateDirectory($pythonFiles)|Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $pipFiles 'pip'))|Out-Null
    [IO.File]::WriteAllText((Join-Path $pythonFiles 'python.exe'),'offline fixture, never executable')
    [IO.File]::WriteAllText((Join-Path $pipFiles 'pip\__init__.py'),'# offline wheel fixture')
    [IO.Compression.ZipFile]::CreateFromDirectory($pythonFiles,(Join-Path $Directory 'python.cache'))
    [IO.Compression.ZipFile]::CreateFromDirectory($pipFiles,(Join-Path $Directory 'pip.cache'))
    [IO.File]::WriteAllText((Join-Path $Directory 'whisper.cache'),'offline wheel fixture')
    return [pscustomobject]@{
        python=[pscustomobject]@{name='python.zip'}
        pip=[pscustomobject]@{name='pip.whl'}
        fasterWhisper=[pscustomobject]@{name='faster_whisper-1.2.1-py3-none-any.whl'}
        pipIndexes=@('https://pypi.tuna.tsinghua.edu.cn/simple','https://repo.huaweicloud.com/repository/pypi/simple')
    }
}
# Every test is offline; accidental network use fails immediately.
function Receive-AsrHttpFile { throw 'TEST FORBIDS NETWORK' }
try {
    Test-Case 'Syntax, interface, BOM and NoRun' {
        foreach ($path in @($installer, $PSCommandPath)) {
            $tokens=$null; $errors=$null
            $ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
            Assert-Asr ($errors.Count -eq 0) "syntax: $path"
            $bytes=[IO.File]::ReadAllBytes($path)
            Assert-Asr ($bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) "UTF8 BOM: $path"
            if ($path -eq $installer) {
                $names=@($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
                Assert-Asr (($names -join ',') -eq 'ProjectRoot,StatusPath,ExistingPython,ExistingModel,NoRun') 'interface'
            }
        }
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $script:testRoot 'unused.ini'))) 'NoRun side effects'
    }
    Test-Case 'Version pins, fixed hashes, domestic mirrors first' {
        $m=Get-AsrManifest
        Assert-Asr ($m.python.name -eq 'python-3.11.9-embed-amd64.zip') 'Python pin'
        Assert-Asr ($m.pip.name -eq 'pip-25.0.1-py3-none-any.whl') 'pip pin'
        Assert-Asr ($m.fasterWhisper.name -eq 'faster_whisper-1.2.1-py3-none-any.whl') 'faster-whisper pin'
        Assert-Asr ($m.pipIndexes[0] -eq 'https://pypi.tuna.tsinghua.edu.cn/simple') 'TUNA first'
        Assert-Asr ($m.modelRevision -match '^[a-f0-9]{40}$') 'revision'
        Assert-Asr ($m.modelFiles.Count -eq 5) 'five model files'
        foreach ($a in @($m.python,$m.pip,$m.fasterWhisper)+$m.modelFiles) {
            Assert-Asr ($a.sha256 -match '^[a-f0-9]{64}$') "hash: $($a.name)"
            $urls=@(Get-AsrSources $a)
            Assert-Asr ($urls.Count -ge 2) 'fallback'
            Assert-Asr ($urls[0] -match '^https://(hf-mirror\.com|pypi\.tuna\.tsinghua\.edu\.cn|mirrors\.huaweicloud\.com)/') 'domestic first'
        }
        foreach ($a in $m.modelFiles) { Assert-Asr ($a.urls[0] -match '/resolve/[a-f0-9]{40}/') 'immutable URL' }
    }
    Test-Case 'UTF16 atomic status, states, Chinese progress, newline sanitization' {
        $path=Join-Path (New-TestDirectory) '状态.ini'
        foreach ($state in @('running','ready','error')) {
            Write-AsrStatus $path $state "下载模型`r`n注入=无效"
            $b=[IO.File]::ReadAllBytes($path)
            $s=[IO.File]::ReadAllText($path)
            Assert-Asr ($b[0] -eq 255 -and $b[1] -eq 254) 'UTF16 LE BOM'
            Assert-Asr ($s.Contains("state=$state`r`n")) 'state'
            Assert-Asr ($s.Contains('message=下载模型  注入=无效')) 'sanitize'
        }
        Assert-Asr (@(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($path)) -Filter '*.tmp').Count -eq 0) 'temporary leak'
    }
    Test-Case 'Atomic replacement failure keeps previous bytes' {
        $path=Join-Path (New-TestDirectory) 'status.ini'
        Write-AsrStatus $path running '上一阶段'
        $before=[Convert]::ToBase64String([IO.File]::ReadAllBytes($path))
        $reader=[IO.File]::Open($path,'Open','Read','Read')
        $threw=$false
        try { Write-AsrStatus $path ready '新阶段' } catch { $threw=$true } finally { $reader.Dispose() }
        Assert-Asr $threw 'locked replacement must fail'
        Assert-Asr ($before -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($path))) 'old status damaged'
    }
    Test-Case 'Reject wrong SHA256, size, Git blob and missing file' {
        $path=Join-Path (New-TestDirectory) 'fixture'
        [IO.File]::WriteAllText($path,'fixture',[Text.UTF8Encoding]::new($false))
        $a=New-TestArtifact $path
        Assert-Asr (Test-AsrArtifact $path $a) 'valid rejected'
        Assert-Asr (!(Test-AsrArtifact ($path+'.missing') $a)) 'missing accepted'
        $a.size++
        Assert-Asr (!(Test-AsrArtifact $path $a)) 'size accepted'
        $a.size--
        $a | Add-Member NoteProperty gitBlob ('0'*40)
        Assert-Asr (!(Test-AsrArtifact $path $a)) 'Git blob accepted'
        $a.PSObject.Properties.Remove('gitBlob')
        [IO.File]::WriteAllText($path,'corrupt')
        Assert-Asr (!(Test-AsrArtifact $path $a)) 'SHA256 accepted'
    }
    Test-Case 'Mirror retry, upstream fallback, verification before publication' {
        $dir=New-TestDirectory; $fixture=Join-Path $dir 'fixture'
        [IO.File]::WriteAllText($fixture,'verified bytes')
        $a=New-TestArtifact $fixture
        $calls=New-Object 'Collections.Generic.List[string]'
        function Receive-AsrHttpFile {
            param($Url,$Destination,$StatusPath,$Label)
            $calls.Add($Url)
            if ($Url -match 'hf-mirror') { throw 'mirror failure' }
            [IO.File]::Copy($fixture,$Destination)
        }
        $status=Join-Path $dir 'status.ini'; $cache=Join-Path $dir 'cache'
        $path=Get-AsrArtifact $a $cache $status '下载模型'
        Assert-Asr ($calls.Count -eq 3) 'retry count'
        Assert-Asr ($calls[0] -eq $a.urls[0] -and $calls[1] -eq $a.urls[0] -and $calls[2] -eq $a.urls[1]) 'source order'
        Assert-Asr (Test-AsrArtifact $path $a) 'unverified publication'
        Assert-Asr (@(Get-ChildItem -LiteralPath $cache -Filter '.part-*' -Force).Count -eq 0) 'partial leak'
        Assert-Asr ([IO.File]::ReadAllText($status).Contains('state=running')) 'progress missing'
    }
    Test-Case 'Repeated validated cache performs no transfer' {
        $dir=New-TestDirectory; $fixture=Join-Path $dir 'fixture'
        [IO.File]::WriteAllText($fixture,'cache fixture'); $a=New-TestArtifact $fixture
        $cache=Join-Path $dir 'cache'; [IO.Directory]::CreateDirectory($cache)|Out-Null
        $cached=Join-Path $cache ($a.sha256+'.cache'); [IO.File]::Copy($fixture,$cached)
        $status=Join-Path $dir 'status.ini'
        Assert-Asr ((Get-AsrArtifact $a $cache $status '下载') -eq $cached) 'cache not reused'
        Assert-Asr ([IO.File]::ReadAllText($status).Contains('复用已校验文件')) 'reuse progress missing'
    }
    Test-Case 'Corrupt transfers fail closed with bounded retries' {
        $dir=New-TestDirectory; $fixture=Join-Path $dir 'fixture'
        [IO.File]::WriteAllText($fixture,'correct'); $a=New-TestArtifact $fixture
        $cache=Join-Path $dir 'cache'; $calls=New-Object 'Collections.Generic.List[string]'
        function Receive-AsrHttpFile { param($Url,$Destination) $calls.Add($Url); [IO.File]::WriteAllText($Destination,'corrupt') }
        $threw=$false
        try { $null=Get-AsrArtifact $a $cache (Join-Path $dir 'status.ini') '下载' } catch { $threw=$true }
        Assert-Asr $threw 'bad bytes accepted'
        Assert-Asr ($calls.Count -eq 4) 'retry budget'
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $cache ($a.sha256+'.cache')))) 'corrupt cache published'
        Assert-Asr (@(Get-ChildItem -LiteralPath $cache -Filter '.part-*' -Force).Count -eq 0) 'temporary leak'
    }
    Test-Case 'Missing hash rejected before transfer' {
        $dir=New-TestDirectory
        $a=[pscustomobject]@{name='bad';sha256='';urls=@('https://hf-mirror.com/test')}
        $threw=$false
        try { $null=Get-AsrArtifact $a $dir (Join-Path $dir 'status.ini') '下载' } catch { $threw=$true }
        Assert-Asr $threw 'missing hash accepted'
        Assert-Asr (@(Get-ChildItem -LiteralPath $dir).Count -eq 0) 'unhashed transfer attempted'
    }
    Test-Case 'Failed repair retains old cached bytes' {
        $dir=New-TestDirectory; $fixture=Join-Path $dir 'fixture'
        [IO.File]::WriteAllText($fixture,'new data'); $a=New-TestArtifact $fixture
        $cache=Join-Path $dir 'cache'; [IO.Directory]::CreateDirectory($cache)|Out-Null
        $old=Join-Path $cache ($a.sha256+'.cache'); [IO.File]::WriteAllText($old,'old data')
        $threw=$false
        try { $null=Get-AsrArtifact $a $cache (Join-Path $dir 'status.ini') '下载' } catch { $threw=$true }
        Assert-Asr $threw 'network denial should fail'
        Assert-Asr ([IO.File]::ReadAllText($old) -eq 'old data') 'old bytes removed'
    }
    Test-Case 'Model completeness, JSON validation and weight hashes' {
        $dir=New-TestDirectory; $m=New-TestModel $dir
        Assert-Asr (Test-AsrModelFiles $dir $m.modelFiles) 'valid fixture rejected'
        [IO.File]::WriteAllText((Join-Path $dir 'config.json'),'{')
        Assert-Asr (!(Test-AsrModelFiles $dir)) 'invalid JSON accepted'
        [IO.File]::WriteAllText((Join-Path $dir 'config.json'),'{}',[Text.UTF8Encoding]::new($false))
        [IO.File]::Delete((Join-Path $dir 'preprocessor_config.json'))
        Assert-Asr (Test-AsrModelFiles $dir) 'existing model without preprocessor rejected'
        Assert-Asr (!(Test-AsrModelFiles $dir $m.modelFiles)) 'new download without preprocessor accepted'
        [IO.File]::WriteAllText((Join-Path $dir 'preprocessor_config.json'),'{}',[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $dir 'model.bin'),'bad')
        Assert-Asr (!(Test-AsrModelFiles $dir $m.modelFiles)) 'corrupt weights accepted'
    }
    Test-Case 'Model staging publication, content reuse and repeat installation' {
        $dir=New-TestDirectory; $fixture=Join-Path $dir 'fixtures'; $m=New-TestModel $fixture
        $base=Join-Path $dir 'runtime\asr'; [IO.Directory]::CreateDirectory($base)|Out-Null
        $status=Join-Path $dir 'status.ini'; $calls=New-Object 'Collections.Generic.List[string]'
        function Receive-AsrHttpFile { param($Url,$Destination) $calls.Add($Url); [IO.File]::Copy((Join-Path $fixture ([Uri]$Url).Segments[-1]),$Destination) }
        $first=Install-AsrModel $m $base $status $dir
        Assert-Asr (Test-AsrModelFiles $first $m.modelFiles) 'incomplete publication'
        Assert-Asr ($calls.Count -eq 3) 'identical JSON should share cache'
        $second=Install-AsrModel $m $base $status $dir
        Assert-Asr ($first -eq $second) 'repeat created another version'
        Assert-Asr ($calls.Count -eq 3) 'repeat used network'
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $dir 'asr-ready.ini'))) 'premature ready manifest'
    }
    Test-Case 'Interrupted model installation keeps old resources and private stage' {
        $dir=New-TestDirectory; $m=New-TestModel (Join-Path $dir 'fixtures')
        $base=Join-Path $dir 'runtime\asr'; $old=Join-Path $base ('tiny-'+$m.modelRevision+'-v1')
        [IO.Directory]::CreateDirectory($old)|Out-Null
        $sentinel=Join-Path $old 'old-resource'; [IO.File]::WriteAllText($sentinel,'preserve')
        $threw=$false
        try { $null=Install-AsrModel $m $base (Join-Path $dir 'status.ini') $dir } catch { $threw=$true }
        Assert-Asr $threw 'download failure ignored'
        Assert-Asr ([IO.File]::ReadAllText($sentinel) -eq 'preserve') 'old resources changed'
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $dir 'asr-ready.ini'))) 'ready exposed during partial install'
        Assert-Asr (@(Get-ChildItem -LiteralPath $base -Directory -Filter '.model-*').Count -eq 1) 'stage not private'
    }
    Test-Case 'Existing Python probe stays read-only and accepts pythonw' {
        $dir=New-TestDirectory; $python=Join-Path $dir 'pythonw.exe'; [IO.File]::WriteAllText($python,'fixture')
        $seen=New-Object 'Collections.Generic.List[string]'
        function Invoke-AsrProcess { param($Python,$Arguments) $seen.Add(($Arguments -join '|')); return 0 }
        Assert-Asr (Test-AsrPython $python) 'pythonw rejected'
        Assert-Asr ($seen[0].StartsWith('-I|-B|-c|')) 'isolation/bytecode flags missing'
        Assert-Asr (!$seen[0].Contains('pip')) 'probe installs packages'
        Assert-Asr (!(Test-AsrPython (Join-Path $dir 'not-python.exe'))) 'not python'
        Assert-Asr (@(Get-ChildItem -LiteralPath $dir).Count -eq 1) 'existing env changed'
    }
    Test-Case 'Load probe uses CPU int8 and explicitly forbids HF fallback' {
        $dir=New-TestDirectory; $null=New-TestModel $dir
        $seen=New-Object 'Collections.Generic.List[string]'
        function Invoke-AsrProcess { param($Python,$Arguments) $seen.Add(($Arguments -join '|')); return 0 }
        Assert-Asr (Test-AsrModelLoad 'fixture' $dir '') 'fixture rejected'
        Assert-Asr ($seen[0].Contains('local_files_only=True')) 'HF fallback possible'
        Assert-Asr ($seen[0].Contains('device="cpu"') -and $seen[0].Contains('compute_type="int8"')) 'CPU int8 missing'
    }
    Test-Case 'Existing-resource reuse, UTF16 ready contract and repeated setup' {
        $dir=New-TestDirectory; $python=Join-Path $dir 'python.exe'; $model=Join-Path $dir 'model'
        [IO.File]::WriteAllText($python,'existing'); $null=New-TestModel $model; $before=(Get-FileHash -LiteralPath $python).Hash
        function Test-AsrPython { param($Python) return (Test-Path -LiteralPath $Python -PathType Leaf) }
        function Test-AsrModelLoad { param($Python,$Directory) return (Test-AsrModelFiles $Directory) }
        function Install-AsrPython { throw 'existing Python not reused' }
        function Install-AsrModel { throw 'existing model not reused' }
        $status=Join-Path $dir 'setup.ini'; $result=Invoke-AsrSetup $dir $status $python $model
        Assert-Asr ($result -eq 0) 'setup failed'
        $readyPath=Join-Path $dir 'runtime\asr-ready.ini'; $ready=Get-AsrReadyValues $readyPath
        Assert-Asr ($ready.python -eq $python -and $ready.model -eq $model) 'wrong ready values'
        $b=[IO.File]::ReadAllBytes($readyPath); Assert-Asr ($b[0] -eq 255 -and $b[1] -eq 254) 'ready UTF16 BOM'
        Assert-Asr ([IO.File]::ReadAllText($status).Contains("state=ready`r`n")) 'state missing'
        Assert-Asr ($before -eq (Get-FileHash -LiteralPath $python).Hash) 'existing Python modified'
        Assert-Asr ((Invoke-AsrSetup $dir $status) -eq 0) 'repeat failed'
    }
    Test-Case 'Setup failure preserves exact old manifest and reports error' {
        $dir=New-TestDirectory; $ready=Join-Path $dir 'runtime\asr-ready.ini'
        Write-AsrAtomicText $ready "[asr]`r`npython=C:\old\python.exe`r`nmodel=C:\old\model`r`n"
        $before=[Convert]::ToBase64String([IO.File]::ReadAllBytes($ready))
        function Test-AsrPython { return $false }
        function Install-AsrPython { throw '模拟运行组件下载失败' }
        $status=Join-Path $dir 'status.ini'
        Assert-Asr ((Invoke-AsrSetup $dir $status) -ne 0) 'failure returned zero'
        Assert-Asr ($before -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($ready))) 'old manifest changed'
        $text=[IO.File]::ReadAllText($status)
        Assert-Asr ($text.Contains('state=error')) 'error state missing'
        Assert-Asr ($text.Contains('模拟运行组件下载失败')) 'failure message missing'
    }
    Test-Case 'Failed model load never publishes ready' {
        $dir=New-TestDirectory
        function Test-AsrPython { return $true }
        function Test-AsrModelLoad { return $false }
        function Install-AsrModel { return 'C:\offline\model' }
        $status=Join-Path $dir 'status.ini'
        Assert-Asr ((Invoke-AsrSetup $dir $status 'C:\offline\python.exe') -ne 0) 'bad load accepted'
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $dir 'runtime\asr-ready.ini'))) 'premature ready'
        Assert-Asr ([IO.File]::ReadAllText($status).Contains('state=error')) 'error missing'
    }
    Test-Case 'Concurrent installer lock fails safely' {
        $dir=New-TestDirectory; $base=Join-Path $dir 'runtime\asr'; [IO.Directory]::CreateDirectory($base)|Out-Null
        $lock=[IO.File]::Open((Join-Path $base 'install.lock'),'OpenOrCreate','ReadWrite','None')
        $status=Join-Path $dir 'status.ini'
        try { $result=Invoke-AsrSetup $dir $status } finally { $lock.Dispose() }
        Assert-Asr ($result -ne 0) 'concurrent install allowed'
        Assert-Asr ([IO.File]::ReadAllText($status).Contains('另一个转写安装任务')) 'lock message missing'
    }
    Test-Case 'Status path collision cannot overwrite ready manifest' {
        $dir=New-TestDirectory; $ready=Join-Path $dir 'runtime\asr-ready.ini'
        Write-AsrAtomicText $ready "[asr]`r`npython=old`r`nmodel=old`r`n"
        Assert-Asr ((Invoke-AsrSetup $dir $ready) -ne 0) 'collision accepted'
        Assert-Asr ([IO.File]::ReadAllText($ready).StartsWith('[asr]')) 'error handler overwrote ready'
    }
    Test-Case 'Explicit local model failure never downloads a replacement or overwrites ready' {
        $dir=New-TestDirectory
        $python=Join-Path $dir 'python.exe'
        [IO.File]::WriteAllText($python,'synthetic')
        $model=Join-Path $dir 'existing model'
        [IO.Directory]::CreateDirectory($model)|Out-Null
        $ready=Join-Path $dir 'runtime\asr-ready.ini'
        Write-AsrAtomicText $ready "[asr]`r`npython=old`r`nmodel=old`r`n"
        $script:replacementDownloads=0
        function Test-AsrPython { return $true }
        function Test-AsrModelLoad { return $false }
        function Install-AsrModel { $script:replacementDownloads++; throw 'unexpected model download' }
        $status=Join-Path $dir 'status.ini'
        Assert-Asr ((Invoke-AsrSetup $dir $status $python $model) -ne 0) 'broken explicit model accepted'
        Assert-Asr ($script:replacementDownloads -eq 0) 'model download consent expanded'
        Assert-Asr ((Get-AsrReadyValues $ready)['model'] -eq 'old') 'old ready changed'
        Assert-Asr ([IO.File]::ReadAllText($status).Contains('未下载替代模型')) 'actionable error missing'
    }
    Test-Case 'Process pipes, Windows escaping and parent environment isolation' {
        $parent=$env:PYTHONPATH; $env:PYTHONPATH='parent-sentinel'
        $code='[Console]::Out.Write((''x'' * 100000)); [Console]::Error.Write((''e'' * 100000)); if ($env:PYTHONPATH) { exit 99 }; exit 7'
        $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
        try {
            $result=Invoke-AsrProcess (Join-Path $PSHOME 'powershell.exe') @('-NoProfile','-NonInteractive','-EncodedCommand',$encoded) 15
            Assert-Asr ($result -eq 7) 'pipe deadlock or environment leak'
            Assert-Asr ($env:PYTHONPATH -eq 'parent-sentinel') 'parent env changed'
            Assert-Asr ((ConvertTo-AsrProcessArgument 'C:\路径 with spaces\') -eq '"C:\路径 with spaces\\"') 'trailing slash'
            Assert-Asr ((ConvertTo-AsrProcessArgument 'a"b') -eq '"a\"b"') 'quote escaping'
        } finally { $env:PYTHONPATH=$parent }
    }
    Test-Case 'Bounded process timeout kills operation' {
        $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('Start-Sleep -Seconds 10'))
        $clock=[Diagnostics.Stopwatch]::StartNew(); $threw=$false
        try { $null=Invoke-AsrProcess (Join-Path $PSHOME 'powershell.exe') @('-NoProfile','-NonInteractive','-EncodedCommand',$encoded) 1 } catch { $threw=$true }
        Assert-Asr $threw 'timeout ignored'
        Assert-Asr ($clock.Elapsed.TotalSeconds -lt 8) 'timeout unbounded'
    }
    Test-Case 'No global Python discovery, registry or global installer' {
        $source=[IO.File]::ReadAllText($installer)
        Assert-Asr ($source -notmatch '(?im)^\s*(winget|choco|scoop|reg\.exe|setx|Start-BitsTransfer)\b') 'global mutation'
        Assert-Asr ($source -notmatch '(?i)Get-Command\s+python|where\.exe\s+python|get-pip\.py') 'requires installed Python'
    }
    Test-Case 'Existing vocabulary.json snapshot without preprocessor is reusable only after load' {
        $dir=New-TestDirectory; $m=New-TestModel $dir
        [IO.File]::Delete((Join-Path $dir 'preprocessor_config.json'))
        [IO.File]::Delete((Join-Path $dir 'vocabulary.txt'))
        [IO.File]::WriteAllText((Join-Path $dir 'vocabulary.json'),'["token"]',[Text.UTF8Encoding]::new($false))
        Assert-Asr (Test-AsrModelFiles $dir) 'vocabulary.json snapshot rejected'
        Assert-Asr (!(Test-AsrModelFiles $dir $m.modelFiles)) 'strict download validation was weakened'
        function Invoke-AsrProcess { return 1 }
        Assert-Asr (!(Test-AsrModelLoad 'fixture' $dir '')) 'failed WhisperModel load accepted'
        function Invoke-AsrProcess { return 0 }
        Assert-Asr (Test-AsrModelLoad 'fixture' $dir '') 'offline load success rejected'
    }
    Test-Case 'Only own GUID stages inside current runtime/asr may move' {
        $root=New-TestDirectory; $base=Join-Path $root 'runtime\asr'
        [IO.Directory]::CreateDirectory($base)|Out-Null
        $stage=Join-Path $base ('.model-'+[Guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($stage)|Out-Null
        $target=Join-Path $base 'tiny-offline-v1'
        Move-AsrStage $root $base $stage $target
        Assert-Asr (Test-Path -LiteralPath $target -PathType Container) 'valid stage move failed'
        $outside=New-TestDirectory
        $threw=$false
        try { Move-AsrStage $root $base $outside (Join-Path $base 'tiny-outside') } catch { $threw=$true }
        Assert-Asr $threw 'outside source allowed'
        Assert-Asr (Test-Path -LiteralPath $outside) 'outside directory changed'
        $threw=$false
        try { Move-AsrStage $root $base $target (Join-Path $base 'tiny-renamed') } catch { $threw=$true }
        Assert-Asr $threw 'existing resource renamed'
        $stage=Join-Path $base ('.model-'+[Guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($stage)|Out-Null
        $threw=$false
        try { Move-AsrStage $root $base $stage (Join-Path $outside 'tiny-escape') } catch { $threw=$true }
        Assert-Asr $threw 'outside destination allowed'
        $threw=$false
        try { Move-AsrStage $root $base $stage $target } catch { $threw=$true }
        Assert-Asr $threw 'existing resource overwritten'
        $threw=$false
        try { Assert-AsrInstallBase $root $outside } catch { $threw=$true }
        Assert-Asr $threw 'foreign installation base accepted'
    }
    Test-Case 'No-Python embedded ZIP and pip-wheel bootstrap with domestic index fallback' {
        $root=New-TestDirectory; $fixtures=Join-Path $root 'fixtures'; $m=New-TestRuntime $fixtures
        $base=Join-Path $root 'runtime\asr'; [IO.Directory]::CreateDirectory($base)|Out-Null
        $calls=New-Object 'Collections.Generic.List[string]'
        function Get-AsrArtifact {
            param($Artifact)
            switch ($Artifact.name) {
                'python.zip' { return (Join-Path $fixtures 'python.cache') }
                'pip.whl' { return (Join-Path $fixtures 'pip.cache') }
                default { return (Join-Path $fixtures 'whisper.cache') }
            }
        }
        function Test-AsrPython {
            param($Python)
            return (Test-Path -LiteralPath (Join-Path ([IO.Path]::GetDirectoryName($Python)) 'Lib\site-packages\installed.marker'))
        }
        function Invoke-AsrProcess {
            param($Python,$Arguments)
            Assert-Asr (Test-Path -LiteralPath (Join-Path ([IO.Path]::GetDirectoryName($Python)) 'Lib\site-packages\pip\__init__.py')) 'pip wheel was not bootstrapped'
            Assert-Asr ($Arguments -contains '--only-binary=:all:' -and $Arguments -contains '--target') 'binary-only isolated install missing'
            $index=$Arguments[[Array]::IndexOf($Arguments,'--index-url')+1]
            $calls.Add($index)
            if ($calls.Count -eq 1) { return 1 }
            $site=$Arguments[[Array]::IndexOf($Arguments,'--target')+1]
            [IO.File]::WriteAllText((Join-Path $site 'installed.marker'),'ok')
            return 0
        }
        $status=Join-Path $root 'status.ini'
        $python=Install-AsrPython $m $base $status $root
        Assert-Asr (Test-AsrPython $python) 'runtime not published'
        Assert-Asr ($calls.Count -eq 2 -and $calls[0] -eq $m.pipIndexes[0] -and $calls[1] -eq $m.pipIndexes[1]) 'index fallback order'
        $published=[IO.Path]::GetDirectoryName($python)
        Assert-Asr ([IO.File]::ReadAllText((Join-Path $published 'python311._pth')).Contains('Lib\site-packages')) 'embedded import path missing'
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $published $m.fasterWhisper.name))) 'bootstrap wheel was not removed'
        Assert-Asr ((Install-AsrPython $m $base $status $root) -eq $python) 'runtime repeat not reused'
        Assert-Asr ($calls.Count -eq 2) 'runtime repeat invoked pip'
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $root 'runtime\asr-ready.ini'))) 'runtime bootstrap published ready too early'
    }
    Test-Case 'Failed runtime bootstrap retains old version and private GUID stage' {
        $root=New-TestDirectory; $fixtures=Join-Path $root 'fixtures'; $m=New-TestRuntime $fixtures
        $base=Join-Path $root 'runtime\asr'; $old=Join-Path $base 'python-3.11.9-fw-1.2.1-v1'
        [IO.Directory]::CreateDirectory($old)|Out-Null
        [IO.File]::WriteAllText((Join-Path $old 'python.exe'),'old version')
        function Get-AsrArtifact {
            param($Artifact)
            switch ($Artifact.name) {
                'python.zip' { return (Join-Path $fixtures 'python.cache') }
                'pip.whl' { return (Join-Path $fixtures 'pip.cache') }
                default { return (Join-Path $fixtures 'whisper.cache') }
            }
        }
        function Test-AsrPython { return $false }
        function Invoke-AsrProcess { return 1 }
        $threw=$false
        try { $null=Install-AsrPython $m $base (Join-Path $root 'status.ini') $root } catch { $threw=$true }
        Assert-Asr $threw 'failed pip install accepted'
        Assert-Asr ([IO.File]::ReadAllText((Join-Path $old 'python.exe')) -eq 'old version') 'old runtime overwritten'
        Assert-Asr (@(Get-ChildItem -LiteralPath $base -Directory -Filter '.python-*').Count -eq 1) 'failed install stage missing'
        Assert-Asr (!(Test-Path -LiteralPath (Join-Path $root 'runtime\asr-ready.ini'))) 'failed bootstrap published ready'
    }
    Test-Case 'Actual script failure exit code is nonzero and keeps ready file intact' {
        $root=New-TestDirectory; $ready=Join-Path $root 'runtime\asr-ready.ini'
        Write-AsrAtomicText $ready "[asr]`r`npython=old`r`nmodel=old`r`n"
        & (Join-Path $PSHOME 'powershell.exe') -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $installer -ProjectRoot $root -StatusPath $ready
        Assert-Asr ($LASTEXITCODE -ne 0) 'entrypoint failure exit code was zero'
        Assert-Asr ([IO.File]::ReadAllText($ready).StartsWith('[asr]')) 'entrypoint damaged ready manifest'
    }
} finally {
    $resolved=[IO.Path]::GetFullPath($script:testRoot)
    $expectedBase=[IO.Path]::GetFullPath((Join-Path $script:testProjectRoot 'runtime\asr')).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($resolved).Equals($expectedBase,[StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved) -match '^\.test-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
        [IO.Directory]::Delete($expectedBase)
        [IO.Directory]::Delete((Join-Path $script:testProjectRoot 'runtime'))
        [IO.Directory]::Delete($script:testProjectRoot)
    }
}
Write-Host ("RESULT: {0} passed, {1} failed; {2} assertions" -f $script:passed,$script:failed,$script:assertions)
Write-Host 'NOT_RUN: real Python/pip installation; full model downloads; real transcription; UI; clean-machine compatibility.'
if ($script:failed -gt 0) { exit 1 }
exit 0
