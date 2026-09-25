param(
	[Parameter(Position = 0, ValueFromRemainingArguments = $true)]
	[string[]]$Command,
	[string]$HostName = "127.0.0.1",
	[int]$Port = 19840
)

if (-not $Command) {
	Write-Error "A monitor command is required"
	exit 2
}

$client = [System.Net.Sockets.TcpClient]::new($HostName, $Port)
$stream = $client.GetStream()
$request = [System.Text.Encoding]::ASCII.GetBytes(($Command -join " ") + "`n")
$stream.Write($request, 0, $request.Length)
$reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)
$response = $reader.ReadLine()
$reader.Dispose()
$client.Dispose()
Write-Output $response
$result = $response | ConvertFrom-Json
if (-not $result.ok) {
    exit 1
}
