# PS4 package keys

RipperPS can turn a PS4 game into a fake package (`.pkg`) that GoldHEN on a
PS4, or etaHEN and ShadowMountPlus on a PS5, install like any other. Making
one needs a few keys. RipperPS doesn't come with them: you supply them in a
key file.

## The file

It's a plain text file. Each line is a name, `=`, and the value in hex. Spaces
in the hex are fine, and `#` starts a comment. Names don't care about case.

```
# My PS4 package keys
PkgPublicKey0 = 0123abcd...
PkgPublicKey1 = ...
```

These are the names, with each value's length in bytes. The values are the
byte arrays of the same name in LibOrbisPkg's `LibOrbisPkg/Util/Keys.cs`
(https://github.com/OpenOrbis/LibOrbisPkg), written as one hex string.

| Name | Bytes | In Keys.cs |
|---|---|---|
| `PkgPublicKey0` to `PkgPublicKey6` | 256 each | `Keys.PkgPublicKeys[0]` to `[6]` |
| `FakeKeyModulus` | 256 | `RSAKeyset.FakeKeyset.Modulus` |
| `DebugRifModulus` | 256 | `RSAKeyset.DebugRifKeyset.Modulus` |
| `DebugRifPrivateExponent` | 256 | `RSAKeyset.DebugRifKeyset.PrivateExponent` |
| `RifDebugKey` | 16 | `Keys.rif_debug_key` |
| `KeystoneHmacKey` | 32 | `Keys.keystone_hmac_key` |
| `KeystoneMacData` | 32 | `Keys.keystone_mac_data` |

The two keystone keys are optional. They're only used for a game folder without
`sce_sys/keystone`, which a dump normally has. Keeping the game's own keystone
keeps its saves working.

If a name is missing, misspelled or the wrong length, RipperPS says which one
and makes no package.

## The packages

- They're games (`gd`), with the content ID from the game's `param.sfo` and the
  all-zero passcode every fake package uses.
- The game's files are stored as they are, not compressed, so the package is a
  little bigger than the folder.
- Install them the usual way: Debug Settings, Package Installer on a PS4 with
  GoldHEN, or etaHEN's or ShadowMountPlus's installer on a PS5.
