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

# Input: routing source, identifier, name, display source, detail, icon (TSV).
# Output keeps routing fields intact and adds one space-aligned display field.
# Do not put tabs inside the display: fzf expands them to tab stops, which can
# consume the space reserved for the last column.
tui_item_columns() {
    terminal_columns=$(tput cols 2>/dev/null || printf '80')
    case $terminal_columns in
        ''|*[!0-9]*) terminal_columns=80 ;;
    esac
    awk -F '\t' -v columns="$terminal_columns" -v detail_limit="${1:-0}" '
        BEGIN {
            accent = "\033[38;2;255;190;111m"
            primary = "\033[38;2;255;255;255m"
            muted = "\033[38;2;170;170;170m"
            reset = "\033[0m"
        }
        function shorten(value, width) {
            if (length(value) <= width) return value
            if (width <= 3) return substr(value, 1, width)
            return substr(value, 1, width - 3) "..."
        }
        {
            rows[NR] = $0
            # Icon and separator have fixed display widths; measure only text.
            label_width = length($4) + 3 + length($3)
            if (label_width > first_width) first_width = label_width
            if (length($5) > detail_width) detail_width = length($5)
        }
        END {
            # Reserve border, pointer, icon, column gap and a safety margin.
            available = columns - 14
            if (detail_limit > 0 && detail_width > detail_limit) detail_width = detail_limit
            if (detail_width + 8 > available) detail_width = int(available / 3)
            if (detail_width < 1) detail_width = 1
            max_first_width = available - detail_width
            if (max_first_width < 1) max_first_width = 1
            if (first_width > max_first_width) first_width = max_first_width
            for (row = 1; row <= NR; row++) {
                split(rows[row], item, "\t")
                source = shorten(item[4], first_width - 4)
                name_width = first_width - length(source) - 3
                if (name_width < 1) name_width = 1
                name = shorten(item[3], name_width)
                # Keep source and name together, padding only at the column edge.
                padding = first_width - length(source) - 3 - length(name)
                if (padding < 0) padding = 0
                printf "%s\t%s\t%s%s  %s · %s%s%s%*s  %s%s%s\n", \
                    item[1], item[2], accent, item[6], source, primary, name, reset, padding, "", \
                    muted, shorten(item[5], detail_width), reset
            }
        }
    '
}
