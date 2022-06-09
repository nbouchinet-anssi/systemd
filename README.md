# Little bit of context and history

The first motivation of this patch was the observation that a `LUKS2 Token` can reference multiple `LUKS2
keyslots`. I then discovered that the converse is also true: multiple `LUKS2 Tokens` can reference a unique `LUKS2
keyslot`. This feature allows us to add factor combination to systemd's disk-encryption tools, i.e.
systemd-crypt{enroll,setup}: as usual, each `LUKS2 Token` represents a given factor, i.e. `systemd-fido2,
systemd-tpm2`, and we are left to find a mechanism to combine them to guard the keyslot they reference.

Shamir's Secret Sharing (SSS) scheme was the first mechanism that poped in my head. It is a quorum-based system to
distribute a secret among `n` participants, and allowing its recovery only once `q` of them recombine their shares;
below, we use `q/n` as a shorthand to denote a given quorum. Sadly there are few well-implemented SSS libraries, so
we started by adding a clean implementation to ANSSI's libecc project [^libecc] before experimenting further.

SSS has a few practical drawbacks:

1. As its name implies, SSS "shares a secret", it is not a secret combination algorithm. Hence, every
   authentication factor has to be able to receive a secret of a `n size` (produced by the algorithm from the
   initial secret) and store it. This is typically not the case of fido2 HMAC Secret Extension, nor it is the case
   for a user. A system where the user has to remember a 256-bit share just cannot work.

2. If `q=1`, then not only every share equivalent to the secret itself, but also every share turns out to be
   identical. This is an edge case of the algorithm, but likewise, we would like to be able to customize the shares
   somehow among the `q` users, even if only one of them is enough to recover the secret.

3. SSS has no weight system: for instance, if you share with quorum `2/n`, any two shares are enough to recover the
   secret; there is no way to make a specific share mandatory. This is unsuitable if you want a policy where the
   disk unlocks only if the TPM2 PCR policy matches *and* either a fido2 token *or* a password is valid too.

I addressed those issues as follows:

1. Instead of distributing the shares as is, we encrypt them based on the factor, using authenticated encryption.
   For example, a user will choose a password, encrypt the share and store it in an apropriate `LUKS2 token`. Thus,
   the systemd factor mechanics are unchanged, they are just used to encrypt their share instead of the `LUKS2
   keyslot wrapping key`.

2. The same solution also solves the second problem: in our example, even with a `1/n` quorum with `n` shared
   passwords, each user will choose her own password and the secret will not be known by anyone directly.

3. In the libecc SSS library, one can share a user-chosen secret (with some constraints that are checked by the
   library), or let the library generate a cryptographically-secure random secret. Remember that we are trying to implement a
   `m+q/n` quorum, with `m` mandatory shares, and `q` among `n` optional shares.  This can be solved using a
   two-level pattern: the first level shares the `LUKS2 keyslot wrapping key` with a `(m+1)/(m+1)` quorum, where
   each of the resulting share is bound to one of the user-chosen mandatory factors, and the last one is a
   user-invisible automatically-generated share. This latter share will in-turn be shared using a user-controlled
   `q/k` quorum, where `q < k`, and the resulting shares are bound to each factor. Hence, if one mandatory share is
   missing, the `LUKS2 keyslot wrapping key` cannot be recovered, allowing one to mark factors as mandatory, and
   others as shared.

sss_generate(2/2)------------------------------------>[Keyslot Encryption Key]
                                                                 / \
                                                                /   \
sss_generate(1/2, glue mandatory share)--->[Glue mandatory share]   [TPM mandatory share]
                                                     / \
                                                    /   \
                                        [FIDO2 share]   [Password share]

# Notes about the patch behavior
During the decryption phase, the patch will first harvest every mandatory factor in the user defined order, it
will next try to harvest the shared factors in the following order: `TPM2, PKCS#11, fido2, password`. Thus, if one
asks a `tpm && (fido2 || password) 1/2` quorum, the `tpm2` mandatory share will be harvested, next the `fido2` one
and the factor harvesting will then stop. The user will have `n tries (default to 3)` for each factor, which are
reseted upon a correct factor validation.

If the user wants to reach the password harvesting phase, in case he forgot or lost his fido2 token for example, he
must define a `timeout=nsec` in the systemd-cryptsetup arguments. He will then have to wait the `nsec` to end in
order to be able to try his password.

During the decryption phase (`systemd-cryptsetup`), the patch will try to fulfill the quorum and stop the factors
harvesting once it is achieved. This means that if you ask a `q/n` quorum, the quorum is fulfilled once that `q`
of the `n` factors are fetched and valid, no more factor harvesting will be performed as the secret can be recovered.

# Notes about what is not implemented yet
1. There is no way to distinguish different fido2 tokens using the `fido2 HMAC Secret Extension`. We have been
   thinking about a way around this limitation but did not implement it yet. It could be done in a dedicated pull
   request.

2. No factor revocation has been implemented for now, but it can easily be.

3. The libecc SSS's implementation provides a way to grow the number of factors for a given quorum. It could be
   used to add new factors to a quorum.

4. It is for now only possible to enroll a unique TPM2 factor and there is no way for the user to feed a crafted
   TPM2 PCR policy to systemd-cryptenroll.

   This can be usefull to predict future PCR values, update the factor quorum before rebooting the computer and
   still be able to fallback to the old boot entry easily.

# Use cases
I will present fiew usecases scenario;

## Strong authentication at boot combined with measured boot
You own a laptop and want it to be unlocked using your computer's `TPM` and a `FIDO2` token, but you forgot your
`FIDO2` token at work, you still want to unlock your laptop without falling back to a single password, you can then enroll
a `TPM2 && (FIDO2 || PASSWORD)` combination.

## Encrypted disk sharing
You are in a company that deploys hardened and controlled computers. You want users to be able to share files using an usb
stick but only between those controlled computers and you want your users to be able to choose to whom they allow
file sharing.

You can write a small tool that encrypts the usb stick using a mandatory secret shared amongst every computer you
control combined to the user's and other's fido2 or pkcs#11 tokens using a 1/n quorum.

The users will then be able to share the usb stick to users they trust only between company controlled computers.

## Quorum based disk encryption
You want to store secret data in an encrypted disk and want it to be decrypted only if a quorum is fulfilled, you
can even add mandatory presences to it.

## Multi TPM2 PCR policy for PCR prediction
Enrolling multiple PCR policy could allow a user to predict future PCR values, update the factor quorum before
rebooting the computer and still be able to fallback to the old boot entry easily.

# Quick patch usage
1. Simple factor combination
```bash
systemd-cryptenroll [factor options ...] BLOCK-DEVICE

systemd-cryptsetup attach VOLUME SOURCEDEVICE [KEY-FILE] [factor options, ...]
```
2. Shared combination
```bash
systemd-cryptenroll [[factor options --shared ...] --quorum=n] BLOCK-DEVICE

systemd-cryptsetup attach VOLUME SOURCEDEVICE [KEY-FILE] [[factor options,shared ...],quorum=n]
```

3. Double combination
```bash
systemd-cryptenroll [[factor options --shared ...] --quorum=n] [factor options ...] BLOCK-DEVICE

systemd-cryptsetup attach VOLUME SOURCEDEVICE [KEY-FILE] [[factor options,shared ...],quorum=n],[factor options, ...]
```

e.g.

## TPM && PASSWORD
```bash
systemd-cryptenroll --tpm2-device=auto --tpm2-pcrs=7,2,3 --password /dev/sdX

/usr/lib/systemd/systemd-cryptsetup attach cryptroot /dev/sdX none tpm2-device=auto,password
```

## PASSWORD 2/4

```bash
systemd-cryptenroll --password --shared --password --shared --password --shared --password --shared --quorum=2 /dev/sdX

/usr/lib/systemd/systemd-cryptsetup attach cryptroot /dev/sdX none password,shared,password,shared,password,shared,password,shared,quorum=2
```

## TPM && (FIDO2 || PASSWORD)
```bash
systemd-cryptenroll --tpm2-device=auto --tpm2-pcrs=7,2,3 --fido2-device=auto --shared --password --shared --quorum=1 /dev/sdX

/usr/lib/systemd/systemd-cryptsetup attach cryptroot /dev/sdX none tpm2-device=auto,fido2-device=auto,shared,password,shared,quorum=1,timeout=15
```

# And last a quick disclaimer
This patch is still a work in progress and I am really open to discussions and improvements.

[^libecc]: https://github.com/ANSSI-FR/libecc
