Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.Text = "AetherFlow Password Field Fixture"
$form.StartPosition = "CenterScreen"
$form.Size = New-Object System.Drawing.Size(520, 220)
$form.TopMost = $true

$label = New-Object System.Windows.Forms.Label
$label.Text = "Password field for AetherFlow UIAutomation mask testing"
$label.AutoSize = $true
$label.Location = New-Object System.Drawing.Point(24, 24)
$form.Controls.Add($label)

$textbox = New-Object System.Windows.Forms.TextBox
$textbox.Name = "PasswordFixtureInput"
$textbox.AccessibleName = "Password fixture input"
$textbox.UseSystemPasswordChar = $true
$textbox.Text = "fixture-secret"
$textbox.Location = New-Object System.Drawing.Point(24, 64)
$textbox.Size = New-Object System.Drawing.Size(450, 28)
$form.Controls.Add($textbox)

$hint = New-Object System.Windows.Forms.Label
$hint.Text = "Run AetherFlow with --password-field-mask while this window is foreground."
$hint.AutoSize = $true
$hint.Location = New-Object System.Drawing.Point(24, 112)
$form.Controls.Add($hint)

$form.Add_Shown({
    $form.Activate()
    $textbox.Focus()
    $textbox.SelectAll()
})

[void]$form.ShowDialog()
