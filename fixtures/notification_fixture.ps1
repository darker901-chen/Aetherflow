Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Local fixture that mimics a foreground notification popup. The window is
# hosted by powershell.exe, so smoke-testing AetherFlow's
# NotificationProducerModule against this fixture means whitelisting
# "powershell.exe" with --notification-mask-process=powershell.exe (or via
# AETHERFLOW_NOTIFICATION_PROCESS_LIST). In real use the whitelist would be
# the messenger's exe (e.g. LINE.exe, Slack.exe, Discord.exe).

$form = New-Object System.Windows.Forms.Form
$form.Text = "AetherFlow Notification Fixture"
$form.StartPosition = "Manual"
$form.Size = New-Object System.Drawing.Size(360, 140)
$form.FormBorderStyle = "FixedToolWindow"
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::FromArgb(245, 245, 245)

$screen = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$form.Location = New-Object System.Drawing.Point(
    ($screen.Right - $form.Width - 24),
    ($screen.Bottom - $form.Height - 24))

$sender = New-Object System.Windows.Forms.Label
$sender.Text = "LINE  -  Wang Xiao Ming"
$sender.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 10)
$sender.AutoSize = $false
$sender.Location = New-Object System.Drawing.Point(16, 12)
$sender.Size = New-Object System.Drawing.Size(320, 22)
$form.Controls.Add($sender)

$body = New-Object System.Windows.Forms.Label
$body.Text = "Tomorrow 3pm meeting -- bring the customer NDA file."
$body.Font = New-Object System.Drawing.Font("Segoe UI", 10)
$body.AutoSize = $false
$body.Location = New-Object System.Drawing.Point(16, 40)
$body.Size = New-Object System.Drawing.Size(320, 60)
$form.Controls.Add($body)

$form.Add_Shown({
    $form.Activate()
})

[void]$form.ShowDialog()
