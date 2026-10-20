!macro customInstall
  WriteRegStr HKCU "Software\Classes\*\shell\FlowDrop" "" "Convert with FlowDrop"
  WriteRegStr HKCU "Software\Classes\*\shell\FlowDrop" "MultiSelectModel" "Player"
  WriteRegStr HKCU "Software\Classes\*\shell\FlowDrop\command" "" '"$INSTDIR\FlowDrop.exe" "%*"'
!macroend

!macro customUnInstall
  DeleteRegKey HKCU "Software\Classes\*\shell\FlowDrop"
!macroend
