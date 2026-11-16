"""Write a small PDF for the clickable-link manual checks (standard library only)."""

from pathlib import Path
import sys


def write_fixture(path: Path) -> None:
    content = (
        "BT /F1 20 Tf 30 750 Td (Clickable links - page 1) Tj ET\n"
        "0 0 1 rg\n"
        "BT /F1 16 Tf 30 700 Td (Jump to page 3) Tj ET\n"
        "BT /F1 16 Tf 30 650 Td (Open https://example.com) Tj ET\n"
        "BT /F1 16 Tf 30 600 Td (Open http://example.com) Tj ET\n"
        "BT /F1 16 Tf 30 550 Td (Email user@example.com) Tj ET\n"
        "0 0 0 rg\n"
        "BT /F1 16 Tf 30 500 Td (Blocked: file:///C:/Windows/notepad.exe) Tj ET\n"
        "BT /F1 16 Tf 30 450 Td (Blocked: ms-settings:) Tj ET\n"
        "BT /F1 16 Tf 30 400 Td (Broken destination: no link) Tj ET\n"
    )
    target = "BT /F1 24 Tf 30 750 Td (Target page 3) Tj ET\n"
    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 6 0 R >> >> /Contents 7 0 R "
        "/Annots [9 0 R 10 0 R 11 0 R 12 0 R 13 0 R 14 0 R 15 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 6 0 R >> >> /Contents 8 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        f"<< /Length {len(content)} >>\nstream\n{content}endstream",
        f"<< /Length {len(target)} >>\nstream\n{target}endstream",
        "<< /Type /Annot /Subtype /Link /Rect [26 692 300 720] /Dest [5 0 R /Fit] >>",
    ]
    for y, uri in [(650, "https://example.com"), (600, "http://example.com"),
                   (550, "mailto:user@example.com"), (500, "file:///C:/Windows/notepad.exe"),
                   (450, "ms-settings:")]:
        objects.append(
            f"<< /Type /Annot /Subtype /Link /Rect [26 {y - 8} 400 {y + 20}] "
            f"/A << /S /URI /URI ({uri}) >> >>"
        )
    objects.append("<< /Type /Annot /Subtype /Link /Rect [26 392 400 420] /Dest [99 0 R /Fit] >>")
    data = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for index, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(f"{index} 0 obj\n{obj}\nendobj\n".encode("ascii"))
    xref = len(data)
    data.extend(f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode("ascii"))
    for offset in offsets[1:]:
        data.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    data.extend(f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


if __name__ == "__main__":
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("out/link-tests/clickable-links.pdf")
    write_fixture(output)
    print(output.resolve())
