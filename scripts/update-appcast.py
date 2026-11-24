#!/usr/bin/env python3
import argparse
import email.utils
import html
import re
import sys
from datetime import datetime, timezone
from pathlib import Path


def existing_windows_item(path: Path) -> str:
    if not path.exists():
        return ""
    content = path.read_text(encoding="utf-8")
    match = re.search(
        r"    <item>\s*.*?sparkle:os=\"windows\".*?    </item>\r?\n?",
        content,
        flags=re.DOTALL,
    )
    return match.group(0) if match else ""


def release_notes_html(notes: str, default: str) -> str:
    return "".join(f"<li>{html.escape(line)}</li>" for line in notes.splitlines() if line.strip()) or default


def main() -> None:
    parser = argparse.ArgumentParser(description="Write the Sparkle appcast for a VoiceStick release.")
    parser.add_argument("--version", required=True)
    parser.add_argument("--zip-url", required=True)
    parser.add_argument("--signature", required=True)
    parser.add_argument("--length", required=True, type=int)
    parser.add_argument("--msi-url")
    parser.add_argument("--msi-length", type=int)
    parser.add_argument("--msi-version")
    parser.add_argument("--msi-release-notes")
    parser.add_argument("--output", default="website/public/appcast.xml")
    parser.add_argument("--release-notes", default="VoiceStick macOS release.")
    args = parser.parse_args()

    if args.length <= 0:
        sys.exit("Error: --length must be greater than 0 for Sparkle updates.")
    if "REPLACE_WITH" in args.signature or not args.signature.strip():
        sys.exit("Error: --signature must be a real Sparkle EdDSA signature.")
    if args.msi_url and (not args.msi_length or args.msi_length <= 0):
        sys.exit("Error: --msi-length must be greater than 0 when --msi-url is set.")

    notes = release_notes_html(args.release_notes, "<li>VoiceStick macOS release.</li>")

    pub_date = email.utils.format_datetime(datetime.now(timezone.utc))
    output_path = Path(args.output)
    windows_item = ""
    if args.msi_url and args.msi_length:
        windows_version = html.escape(args.msi_version or args.version)
        windows_notes = release_notes_html(
            args.msi_release_notes if args.msi_release_notes is not None else args.release_notes,
            "<li>VoiceStick Windows release.</li>",
        )
        windows_item = f"""    <item>
      <title>Version {windows_version}</title>
      <description><![CDATA[
        <ul>
          {windows_notes}
        </ul>
      ]]></description>
      <pubDate>{pub_date}</pubDate>
      <enclosure
        url="{html.escape(args.msi_url)}"
        sparkle:os="windows"
        sparkle:version="{windows_version}"
        sparkle:shortVersionString="{windows_version}"
        sparkle:installerArguments="/qb!+ /norestart VOICESTICK_RELAUNCH=1"
        length="{args.msi_length}"
        type="application/octet-stream"
      />
    </item>
"""
    else:
        windows_item = existing_windows_item(output_path)

    content = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>VoiceStick</title>
    <link>https://78.github.io/voicestick/appcast.xml</link>
    <description>VoiceStick app updates</description>
    <language>zh-CN</language>
{windows_item}    <item>
      <title>Version {html.escape(args.version)}</title>
      <description><![CDATA[
        <ul>
          {notes}
        </ul>
      ]]></description>
      <pubDate>{pub_date}</pubDate>
      <sparkle:minimumSystemVersion>12.0</sparkle:minimumSystemVersion>
      <enclosure
        url="{html.escape(args.zip_url)}"
        sparkle:os="macos"
        sparkle:version="{html.escape(args.version)}"
        sparkle:shortVersionString="{html.escape(args.version)}"
        sparkle:edSignature="{html.escape(args.signature)}"
        length="{args.length}"
        type="application/octet-stream"
      />
    </item>
  </channel>
</rss>
"""
    output_path.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
