# shellcheck shell=sh
# Shared terminal colors. Resolve this file through each command's real path so
# both repository scripts and the installer's symlinks use the same palette.
export NEWT_COLORS='root=white,black
border=white,black
window=white,black
shadow=black,black
title=yellow,black
button=black,yellow
actbutton=black,brown
compactbutton=white,black
checkbox=white,black
actcheckbox=black,yellow
entry=white,black
label=white,black
listbox=white,black
actlistbox=black,yellow
sellistbox=white,black
actsellistbox=black,yellow
textbox=white,black
acttextbox=black,yellow
emptyscale=white,black
fullscale=black,yellow
helpline=white,black
roottext=white,black'

tui_fzf() {
    fzf --color='bg:#222226,bg+:#38383c,fg:#ffffff,fg+:#ffffff,hl:#ffbe6f,hl+:#ffa348,prompt:#ffbe6f,pointer:#ffbe6f,marker:#ffbe6f,border:#55555a,label:#aaaaaa,info:#aaaaaa,spinner:#ffbe6f,header:#aaaaaa' "$@"
}
