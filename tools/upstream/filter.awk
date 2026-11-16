# Drops the lines whose path (the last tab-separated field) tools/upstream/removed.txt lists. Usage:
#   ... | awk -f tools/upstream/filter.awk tools/upstream/removed.txt -
BEGIN { FS = "\t" }
FNR == NR { if ($0 !~ /^(#|$)/) rm[++n] = $0; next }
{
    for (i = 1; i <= n; i++)
        if ($NF == rm[i] || (rm[i] ~ /\/$/ && index($NF, rm[i]) == 1)) next
    print
}
