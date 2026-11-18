# Convert Debian installer Ethernet/Wi-Fi stanzas into nmcli property/value pairs.
# Reject unsupported settings before activating any profiles or switching managers.
function reject(reason) {
    print "Cannot import ifupdown configuration" (iface ? " for " iface : "") ": " reason > "/dev/stderr"
    failed=1
    exit 1
}
function option(key, value) { print key "\n" value >> file }
function prefix(mask,    bits, i, octets, bit, zero, n) {
    if (mask !~ /\./) return mask
    if (split(mask,octets,".") != 4) reject("invalid IPv4 netmask")
    for (i=1; i<=4; i++) {
        n=octets[i]
        if (n !~ /^[0-9]+$/ || n>255) reject("invalid IPv4 netmask")
        for (bit=128; bit>=1; bit/=2) {
            if (int(n/bit)%2) { if (zero) reject("noncontiguous IPv4 netmask"); bits++ }
            else zero=1
        }
    }
    return bits+0
}
function ip_config(family,    method,ip,addr,mask,octet,dns) {
    method=methods[iface,family]
    ip=(family=="inet" ? "ipv4" : "ipv6")
    if (!method) {
        option(ip ".method", family=="inet" ? "disabled" : "auto")
        return
    }
    if (method=="static") {
        addr=values[iface,family,"address"]
        mask=values[iface,family,"netmask"]
        if (!addr) reject("static address missing")
        if (mask!="" && addr ~ /\//) reject("both CIDR prefix and netmask specified")
        if (addr !~ /\//) {
            # ifupdown infers a classful netmask for bare IPv4 addresses.
            if (mask=="" && family=="inet") {
                split(addr,octet,".")
                mask=(octet[1]<128 ? 8 : octet[1]<192 ? 16 : octet[1]<224 ? 24 : 32)
            }
            addr=addr "/" (mask!="" ? prefix(mask) : 128)
        }
        option(ip ".addresses",addr)
        option(ip ".method","manual")
    } else {
        if (values[iface,family,"address"] || values[iface,family,"netmask"]) reject("address on automatic interface")
        option(ip ".method",(family=="inet6" && method=="dhcp") ? "dhcp" : "auto")
    }
    if (values[iface,family,"gateway"]) option(ip ".gateway",values[iface,family,"gateway"])
    if (values[iface,family,"metric"]!="") option(ip ".route-metric",values[iface,family,"metric"])
    dns=values[iface,family,"dns-nameservers"]
    if (dns) option(ip ".dns",dns)
    if (values[iface,family,"dns-search"]) option(ip ".dns-search",values[iface,family,"dns-search"])
}
/^[[:space:]]*(#|$)/ { next }
$1=="@end" {iface=""; next}
$1=="auto" || $1=="allow-auto" || $1=="allow-hotplug" {
    for (i=2;i<=NF;i++) automatic[$i]=1
    iface=""; next
}
$1=="iface" {
    iface=$2; family=$3
    if (iface ~ /[.][0-9]+$/ || iface ~ /^vlan[0-9]+$/) reject("VLAN needs a native NetworkManager profile")
    if (NF!=4 || iface !~ /^[[:alnum:]_][[:alnum:]_.-]*$/) reject("unsupported interface declaration")
    if (iface=="lo" && $4=="loopback") next
    if (iface=="lo" || (family!="inet" && family!="inet6") ||
        ($4!="dhcp" && $4!="static" && !(family=="inet6" && $4=="auto"))) reject("unsupported address method")
    if (methods[iface,family]) reject("multiple stanzas for one address family")
    methods[iface,family]=$4; devices[iface]=1; next
}
{
    key=$1
    if (!iface || iface=="lo" || key !~ /^(address|netmask|gateway|metric|dns-nameservers|dns-search|mtu|wpa-ssid|wpa-psk|wireless-essid)$/)
        reject("unsupported option or directive " key)
    sub(/^[[:space:]]*[^[:space:]]+[[:space:]]+/, "")
    if ($0 ~ /\\$/ || !length($0) || values[iface,family,key]) reject("ambiguous or repeated " key)
    if ((key=="wpa-ssid" || key=="wpa-psk") && /^".*"$/) $0=substr($0,2,length($0)-2)
    values[iface,family,key]=$0
    if (key=="wpa-ssid" || key=="wireless-essid") {
        if ($0=="" || ((iface in ssids) && ssids[iface]!=$0)) reject("empty or conflicting SSID")
        ssids[iface]=$0
    }
    if (key=="wpa-psk") {
        if ((iface in secrets) && secrets[iface]!=$0) reject("conflicting WPA keys")
        secrets[iface]=$0
    }
    if (key=="mtu") {
        if ((iface in mtus) && mtus[iface]!=$0) reject("conflicting MTUs")
        mtus[iface]=$0
    }
}
END {
    if (failed) exit 1
    for (iface in devices) {
        file=dir "/" iface ".args"
        option("connection.autoconnect",automatic[iface] ? "yes" : "no")
        # Prefer the explicit Debian configuration over an auto-created wired profile.
        option("connection.autoconnect-priority",1)
        ip_config("inet"); ip_config("inet6")
        if (mtus[iface]) option((ssids[iface]!="" ? "wifi" : "ethernet") ".mtu",mtus[iface])
        if (ssids[iface]!="") print ssids[iface] > (file ".wifi")
        if (iface in secrets) {
            if (ssids[iface]=="" || length(secrets[iface])<8 || length(secrets[iface])>64 ||
                secrets[iface] ~ /[\\[:cntrl:]]|^[[:space:]]|[[:space:]]$/ ||
                (length(secrets[iface])==64 && secrets[iface] ~ /[^[:xdigit:]]/)) reject("unsupported WPA key format")
            option("wifi-sec.key-mgmt","wpa-psk")
            print "psk=" secrets[iface] > (file ".psk")
        }
        close(file)
    }
}
