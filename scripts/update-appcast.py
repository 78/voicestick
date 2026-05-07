#!/usr/bin/env python3
import argparse
import email.utils
import html
import re
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


def main() -> None:
    parser = argparse.ArgumentParser(description="Write the Sparkle appcast for a VoiceStick release.")
    parser.add_argument("--version", required=True)
    parser.add_argument("--zip-url", required=True)
    parser.add_argument("--signature", required=True)
    parser.add_argument("--length", required=True, type=int)
    parser.add_argument("--msi-url")
    parser.add_argument("--msi-length", type=int)
    parser.add_argument("--output", default="website/public/appcast.xml")
    parser.add_argument("--release-notes", default="VoiceStick macOS release.")
    args = parser.parse_args()

    notes = "".join(f"<li>{html.escape(line)}</li>" for line in args.release_notes.splitlines() if line.strip())
    if not notes:
        notes = "<li>VoiceStick macOS release.</li>"

    pub_date = email.utils.format_datetime(datetime.now(timezone.utc))
    output_path = Path(args.output)
    windows_item = ""
    if args.msi_url and args.msi_length:
        windows_item = f"""    <item>
      <title>Version {html.escape(args.version)}</title>
      <description><![CDATA[
        <ul>
          {notes}
        </ul>
      ]]></description>
      <pubDate>{pub_date}</pubDate>
      <enclosure
        url="{html.escape(args.msi_url)}"
        sparkle:os="windows"
        sparkle:version="{html.escape(args.version)}"
        sparkle:shortVersionString="{html.escape(args.version)}"
        sparkle:installerArguments="/passive"
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
