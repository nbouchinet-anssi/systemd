#!/usr/bin/env bash
set -e

src="$(dirname "$0")/testdata/test-disk-encryption"

function feed_mandatory_passwords() {
    local n
    n="${1}"
    readonly n

    for s in $(seq "${n}"); do
        mandatory_passwords+=("$(tr -dc [:alnum:] </dev/urandom | head -c 16)")
    done
}

feed_shared_passwords() {
    local n
    n="${1}"
    readonly n

    for s in $(seq "${n}"); do
        shared_passwords+=("$(tr -dc [:alnum:] </dev/urandom | head -c 16)")
    done
}

build_luks_header() {
	#echo "Creating a new encrypted disk..."
    if [[ -L "/dev/mapper/${mapper_name}" ]]; then
	    cryptsetup luksClose ${mapper_name} || echo "cryptsetup luksClose Failure"
    fi

    if [[  -f file.img ]]; then
        rm file.img
    fi
    dd if=/dev/zero of=~/file.img bs=100M count=1
    device=$(losetup -fP --show ~/file.img)

    cryptsetup luksFormat --batch-mode --use-random --pbkdf pbkdf2 --pbkdf-force-iterations 1000 \
        "${device}" < <(echo "${base_secret}")
    cryptsetup luksOpen ${device} ${mapper_name} < <(echo "${base_secret}")

	#echo "Creating an ext4 file system in the new encrypted disk..."
	mkfs.ext4 /dev/mapper/${mapper_name}
	#echo "Mounting the new partition at /mnt.."
	mount /dev/mapper/${mapper_name} /mnt
}

function encrypt() {
    expect <<- EOF || echo "Encryption Failure"
spawn systemd-cryptenroll $(
if [[ "${tpm2_device}" ]]; then
    echo -n "--tpm2-device=${tpm2_device} --tpm2-pcrs=${tpm2_pcrs} "
fi
if [[ "${fido2_device}" ]]; then
    echo -n "--fido2-device=${fido2_device} --fido2-with-client-pin=no --fido2-with-user-presence=no "
fi
for i in "${mandatory_passwords[@]}"; do
    echo -n "--password "
done
if [[ ${quorum_size} -gt 0 ]]; then
    for i in "${shared_passwords[@]}"; do
        echo -n "--password --shared "
    done
    echo -n "--quorum=${quorum_size} "
fi
echo " ${device}"
)
match_max 100000
expect -re "(.*)current passphrase(.*)" { send -- "${base_secret}\\r" }
$(
if [[ "${fido2_device}" ]]; then
    echo expect -re '"(.*)security token PIN(.*)"' { send -- \""${fido2_pin}"\\r\" }
fi

if [[ ${n_mandatory_password} -gt 0 ]]; then
    for i in "${mandatory_passwords[@]}"; do
        echo expect -re '"(.*)new passphrase(.*)"' { send -- \""${i}"\\r\" }
        echo expect -re '"(.*)new passphrase(.*)repeat(.*)"' { send -- \""${i}"\\r\" }
    done
fi

if [[ ${n_shared_password} -gt 0 ]] && [[ ${quorum_size} ]]; then
    for i in "${shared_passwords[@]}"; do
        echo expect -re '"(.*)new passphrase(.*)"' { send -- \""${i}"\\r\" }
        echo expect -re '"(.*)new passphrase(.*)repeat(.*)"' { send -- \""${i}"\\r\" }
    done
fi
)
expect eof
EOF

}

function decrypt() {
    expect <<-EOF || echo "Decryption Failure"
set timeout -1
spawn /usr/lib/systemd/systemd-cryptsetup attach ${mapper_name} ${device} none $(
if [[ "${tpm2_device}" ]]; then
    echo -n "tpm2-device=${tpm2_device},tpm2-pcrs=${tpm2_pcrs},"
fi
if [[ "${fido2_device}" ]]; then
    echo -n "fido2-device=${fido2_device},"
fi
if [[ ${n_mandatory_password} -gt 0 ]]; then
    for i in "${mandatory_passwords[@]}"; do
        echo -n "password,"
    done
fi
if [[ ${quorum_size} -gt 0 ]]; then
    for i in "${shared_passwords[@]}"; do
        echo -n "password,shared,"
    done
    echo -n "quorum=${quorum_size},"
fi
)keyslot=1
match_max 100000
$(
if [[ "${fido2_device}" ]]; then
    echo expect -re '"(.*)security token PIN(.*)"' { send -- \""${fido2_pin}"\\r\" }
fi
if [[ ${n_mandatory_password} -gt 0 ]]; then
for i in "${mandatory_passwords[@]}"; do
    echo expect -re '"(.*)passphrase(.*)"' { send -- \""${i}"\\r\" }
done
fi
for i in "${!shared_passwords[@]}"; do
    [[ ${i} -lt ${quorum_size} ]] || break
    echo expect -re '"(.*)passphrase(.*)"' { send -- \""${shared_passwords[i]}"\\r\" }
done
)
expect eof
EOF
}

function worth_testing() {
    if [[ ${n_shared_password} -eq 1 ]]; then
        echo SKIP
        return
    fi
    if [[ ${n_mandatory_password} -eq 0 ]] && [[ ${n_shared_password} -eq 0 ]] && ! [[ ${tpm2_device} ]] && ! [[ ${fido2_device} ]]; then
        echo SKIP
        return
    fi
    echo CONTINUE
}

function tests() {
    if [[ $(worth_testing) == "SKIP" ]]; then
        tput setaf 2; echo "SUCCESS"; tput sgr0
        return 0
    fi

    device=""
    mapper_name="$(tr -dc [:alpha:] </dev/urandom | head -c 8)"
    base_secret="$(tr -dc [:alnum:] </dev/urandom | head -c 16)"

	build_luks_header || exit 1
	#echo "Writing /mnt/fichier.txt..."
	echo "HI I AM AN ENCRYPTED FILE" >> /mnt/fichier.txt
	before=$(cat /mnt/fichier.txt)
	umount -R /mnt
	cryptsetup luksClose ${mapper_name}
	echo "Adding a new encryption method to the disk..."
	encrypt
	echo "Using the new encryption method of the disk in order to decrypt it..."
	decrypt
	echo "Mounting the decrypted disk"
	mount /dev/mapper/${mapper_name} /mnt
	echo "Reading /mnt/fichier.txt"
	after=$(cat /mnt/fichier.txt)
	if [[ ${before} == ${after} ]]; then
        tput setaf 2; echo "SUCCESS"; tput sgr0
	else
        tput setaf 1; echo "FAILURE"; tput sgr0
    fi
	umount -R /mnt || echo "Failed to unmount"
    if [[ -L "/dev/mapper/${mapper_name}" ]]; then
        cryptsetup luksClose ${mapper_name} || echo "cryptsetup luksClose Failure"
    fi
    losetup --detach "${device}"
}

function test_legacy() {
    unset n_mandatory_password n_shared_password quorum_size shared_passwords mandatory_passwords fido2_device fido2_pin tpm2_device tpm2_pcrs
    echo "*** Running legacy password encryption"
    for i in {0..1}; do
        n_mandatory_password=1
        n_shared_password=0
        shared_passwords=()
        mandatory_passwords=()

        feed_mandatory_passwords ${n_mandatory_password}
        feed_shared_passwords ${n_shared_password}

        tests
    done
}

function test_password() {
    unset n_mandatory_password n_shared_password quorum_size shared_passwords mandatory_passwords fido2_device fido2_pin tpm2_device tpm2_pcrs
    test_password_combination
}

function test_password_combination() {
    unset n_mandatory_password n_shared_password quorum_size shared_passwords mandatory_passwords fido2_device fido2_pin tpm2_device tpm2_pcrs
    echo "*** Running password combination encryption"
    for i in {0..10}; do
        n_mandatory_password=$(( RANDOM % 16 ))
        n_shared_password=$(( RANDOM % 16 ))
        quorum_size=$((n_shared_password - 1))
        shared_passwords=()
        mandatory_passwords=()

        feed_mandatory_passwords ${n_mandatory_password}
        feed_shared_passwords ${n_shared_password}

        tests
    done
}

function test_tpm() {
    unset n_mandatory_password n_shared_password quorum_size shared_passwords mandatory_passwords fido2_device fido2_pin tpm2_device tpm2_pcrs
    echo "*** Running tpm2 encryption"
    for i in {0..10}; do
        tpm2_device="auto"
        tpm2_pcrs="7"

        tests
    done
}

function test_fido2() {
    unset n_mandatory_password n_shared_password quorum_size shared_passwords mandatory_passwords fido2_device fido2_pin tpm2_device tpm2_pcrs
    echo "*** Running fido2 encryption"
    for i in {0..10}; do
    fido2_device="auto"
    fido2_pin=""

    tests
    done
}

function test_pkcs11() {
    echo "pkcs11 tests not supported"
}

function test_factor_combination() {
    unset n_mandatory_password n_shared_password quorum_size shared_passwords mandatory_passwords fido2_device fido2_pin tpm2_device tpm2_pcrs
    echo "*** Running any combination encryption"
    for i in {0..10}; do
        n_mandatory_password=$(( RANDOM % 12 ))
        n_shared_password=$(( RANDOM % 12 ))
        quorum_size=$((n_shared_password - 1))
        shared_passwords=()
        mandatory_passwords=()

        fido2_device="auto"
        fido2_pin=""

        tpm2_device="auto"
        tpm2_pcrs="7"

        feed_mandatory_passwords ${n_mandatory_password}
        feed_shared_passwords ${n_shared_password}

        tests
    done
}

function main() {
    test_legacy || exit 1
    test_password || exit 1
    test_tpm || exit 1
    #test_fido2 || exit 1
    #test_factor_combination || exit 1
    #test_pkcs11 || exit 1
}

main || exit 1
