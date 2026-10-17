# Third-party files pinned for packaging

Packaging never downloads (ADR 0024). A few licence texts, notices and source files that a release must
ship come from neither a vcpkg source archive nor the toolchain, so their bytes are kept here, each
exactly as its published source holds it. `installer\license-policy.json` (`thirdPartySources`) names
each folder's repository and commit and pins every file by its git blob id; packaging
(`Get-ThirdPartySourceFile`, `installer\scripts\LucentPackaging.psm1`) refuses a file whose bytes do
not have that id (or the same text with other line endings, which gives the blob's bytes).
`.gitattributes` stores the folder byte for byte.

Taken for the gate-8 review, round 2 (ADR 0024), with `git fetch` of the named commit, whose objects
git verifies by their hashes:

| Folder | Published source | What packaging takes from it |
|---|---|---|
| `inno-setup-6.7.3` | <https://github.com/jrsoftware/issrc>, tag `is-6_7_3` (signed by Martijn Laan), commit `4adf37ed7f3fd2bd11c6836ba056e3de170fbabf`, the source of the Inno Setup 6.7.3 compiler that makes Lucent Setup | `Components/UniPs/LICENSE.md`: the licence of RemObjects Pascal Script, which the setup engine (inside Setup and the uninstaller) contains. `Projects/Src/Compression.LZMA1SmallDecompressor/LzmaDecode/`: the source of the LZMA SDK 4.40 decoder in Setup's loader (`LzmaDecodeInno.c` includes `LzmaDecodeSize.c`, which includes `LzmaDecode.h` and `LzmaTypes.h`; `compile.bat` compiles it), which Lucent ships beside its notice because its files offer it under the GNU LGPL or the Common Public License |
| `apache-tika-408c26e1` | <https://github.com/apache/tika>, commit `408c26e1e03e018a623e732dff6fb047a2fb8e19`, the version of Tika's MIME type definitions that Qt's SBOM records inside `Qt6Core.dll` | `NOTICE.txt`: the lines of Apache Tika's NOTICE that pertain to `tika-mimetypes.xml` (Apache-2.0 section 4(d)) |

A new Inno Setup or Qt version replaces a folder in the same review that records the new version in
the policy: fetch the new commit, copy the files with `git cat-file blob <commit>:<path>`, and record
each file's blob id (`git rev-parse <commit>:<path>`).
