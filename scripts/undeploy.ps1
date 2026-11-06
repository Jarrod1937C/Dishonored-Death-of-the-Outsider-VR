# Removes the mod from the game folder (restores vanilla).
param([string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Dishonored_DeathOfTheOutsider')
Remove-Item "$GameDir\dinput8.dll", "$GameDir\DOTOVR.dll", "$GameDir\DOTOVR.pdb", "$GameDir\DOTOVR.log" -Force -ErrorAction SilentlyContinue
"Removed."
