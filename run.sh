mkdir -p /tmp/emulated_tpm/systemd
swtpm socket --tpmstate dir=/tmp/emulated_tpm/systemd --ctrl type=unixio,path=/tmp//emulated_tpm/systemd/swtpm-sock --log level=20 --tpm2 --daemon
sudo mkosi qemu \
    -chardev socket,id=chrtpm,path=/tmp/emulated_tpm/systemd/swtpm-sock \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    -usb -device usb-host,vendorid=0x1050,productid=0x0407
