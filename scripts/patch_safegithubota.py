import os
from pathlib import Path

Import("env")


def patch_file(path: Path, replacements):
    if not path.exists():
        print(f"[patch_safegithubota] Skipping missing file: {path}")
        return False

    original = path.read_text(encoding="utf-8")
    updated = original

    for old, new in replacements:
        if old in updated:
            updated = updated.replace(old, new)

    if updated != original:
        path.write_text(updated, encoding="utf-8")
        print(f"[patch_safegithubota] Patched: {path}")
        return True

    print(f"[patch_safegithubota] Already up-to-date: {path}")
    return False


def main():
    pio_env = env.subst("$PIOENV")
    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / pio_env
    base = libdeps_dir / "SafeGithubOTA" / "src"

    github_client = base / "SGO_GitHubClient.cpp"
    provisioning = base / "SGO_Provisioning.cpp"

    patch_file(
        github_client,
        [
            (
                "setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);",
                "setCACertBundle(rootca_crt_bundle_start);",
            ),
            (
                '"Authorization: Bearer " + pat + "\\r\\n" +\n                 "Accept: application/vnd.github+json\\r\\n" +',
                '((pat != nullptr && pat[0] != \'\\0\') ? (String("Authorization: Bearer ") + pat + "\\r\\n") : String("")) +\n                 "Accept: application/vnd.github+json\\r\\n" +',
            ),
            (
                '"Authorization: Bearer " + pat + "\\r\\n" +\n                 "Accept: application/octet-stream\\r\\n" +',
                '((pat != nullptr && pat[0] != \'\\0\') ? (String("Authorization: Bearer ") + pat + "\\r\\n") : String("")) +\n                 "Accept: application/octet-stream\\r\\n" +',
            ),
        ],
    )

    patch_file(
        provisioning,
        [
            (
                "if (owner.length() == 0 || repo.length() == 0 ||\n        pat.length() == 0 || bin.length() == 0) {",
                "if (owner.length() == 0 || repo.length() == 0 || bin.length() == 0) {",
            ),
            (
                "bool has = prefs.getString(KEY_OWNER, \"\").length() > 0 &&\n               prefs.getString(KEY_REPO, \"\").length() > 0 &&\n               prefs.getString(KEY_PAT, \"\").length() > 0 &&\n               prefs.getString(KEY_BIN, \"\").length() > 0;",
                "bool has = prefs.getString(KEY_OWNER, \"\").length() > 0 &&\n               prefs.getString(KEY_REPO, \"\").length() > 0 &&\n               prefs.getString(KEY_BIN, \"\").length() > 0;",
            ),
            (
                "if (owner.length() == 0 || repo.length() == 0 ||\n            pat.length() == 0 || bin.length() == 0) {",
                "if (owner.length() == 0 || repo.length() == 0 || bin.length() == 0) {",
            ),
        ],
    )


main()
