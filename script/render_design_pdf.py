#!/usr/bin/env python3
"""Render the architecture source as a Chinese PDF with a vector overview diagram."""
from pathlib import Path
import re
import sys
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.platypus import (BaseDocTemplate, Flowable, Frame, PageTemplate,
                                Paragraph, Spacer, PageBreak, Preformatted,
                                KeepTogether)


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "architecture.md"
OUTPUT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/taycan_design_document.pdf"
OUTPUT.parent.mkdir(parents=True, exist_ok=True)

FONT = "STSong-Light"
pdfmetrics.registerFont(UnicodeCIDFont(FONT))
BLUE = colors.HexColor("#275D8C")
PALE = colors.HexColor("#EEF4F8")
INK = colors.HexColor("#263442")
MUTED = colors.HexColor("#627386")


class ArchitectureDiagram(Flowable):
    def __init__(self, width=500, height=230):
        super().__init__()
        self.width = width
        self.height = height

    def draw(self):
        c = self.canv
        c.saveState()
        c.setFont(FONT, 8.5)

        def box(x, y, w, h, title, body, fill=PALE, stroke=BLUE):
            c.setFillColor(fill)
            c.setStrokeColor(stroke)
            c.setLineWidth(0.8)
            c.roundRect(x, y, w, h, 6, fill=1, stroke=1)
            c.setFillColor(INK)
            c.setFont(FONT, 9)
            c.drawCentredString(x+w/2, y+h-14, title)
            c.setFillColor(MUTED)
            c.setFont(FONT, 7.5)
            for i, line in enumerate(body.split("\n")):
                c.drawCentredString(x+w/2, y+h-28-11*i, line)

        def arrow(x1, y1, x2, y2):
            c.setStrokeColor(BLUE)
            c.setFillColor(BLUE)
            c.setLineWidth(1)
            c.line(x1, y1, x2, y2)
            c.line(x2, y2, x2-5, y2+2.5)
            c.line(x2, y2, x2-5, y2-2.5)

        box(0, 164, 100, 52, "调用线程", "同步 Get / Put / Delete")
        box(132, 164, 112, 52, "KVEngine 适配层", "接口转换与错误传播")
        box(278, 164, 106, 52, "F2 Session", "前台操作与索引更新")
        box(418, 164, 82, 52, "确认", "返回调用方", fill=colors.HexColor("#EAF5ED"), stroke=colors.HexColor("#39824B"))
        arrow(100, 190, 132, 190)
        arrow(244, 190, 278, 190)
        arrow(384, 190, 418, 190)

        box(20, 82, 120, 54, "HybridLog", "追加 KV / 删除记录\n热端内存页与冷端磁盘段")
        box(190, 82, 120, 54, "提交边界", "连续 serial + CRC footer\n数据文件 fdatasync")
        box(360, 82, 120, 54, "持久前沿", "推进 stable tail\n发布索引后 ACK")
        arrow(140, 109, 190, 109)
        arrow(310, 109, 360, 109)

        box(24, 4, 130, 48, "内存 Hash Index", "tag + HybridLog address\n读时精确比较完整 Key")
        box(184, 4, 130, 48, "磁盘 ColdIndex", "索引页按需换入\n预算受 mem_limit_bytes 控制")
        box(344, 4, 136, 48, "恢复 / 回收", "Checkpoint 先发布新根\n再回收旧段")
        arrow(154, 28, 184, 28)
        arrow(314, 28, 344, 28)
        c.setDash(3, 2)
        c.line(78, 82, 78, 52)
        c.line(250, 82, 250, 52)
        c.line(420, 82, 420, 52)
        c.restoreState()


def page(canvas, doc):
    canvas.saveState()
    w, h = A4
    canvas.setStrokeColor(colors.HexColor("#DCE4EA"))
    canvas.line(42, 35, w-42, 35)
    canvas.setFillColor(MUTED)
    canvas.setFont(FONT, 8)
    canvas.drawString(42, 22, "Taycan 磁盘 KV 引擎 · 架构设计候选")
    canvas.drawRightString(w-42, 22, f"{doc.page}")
    canvas.restoreState()


styles = getSampleStyleSheet()
styles.add(ParagraphStyle(name="DocTitleCN", fontName=FONT, fontSize=21, leading=29,
                          textColor=BLUE, alignment=TA_CENTER, spaceAfter=8))
styles.add(ParagraphStyle(name="SubtitleCN", fontName=FONT, fontSize=9, leading=14,
                          textColor=MUTED, alignment=TA_CENTER, spaceAfter=18))
styles.add(ParagraphStyle(name="H1CN", fontName=FONT, fontSize=15, leading=21,
                          textColor=BLUE, spaceBefore=12, spaceAfter=6, keepWithNext=True))
styles.add(ParagraphStyle(name="H2CN", fontName=FONT, fontSize=11, leading=16,
                          textColor=INK, spaceBefore=8, spaceAfter=4, keepWithNext=True))
styles.add(ParagraphStyle(name="BodyCN", fontName=FONT, fontSize=9, leading=15,
                          textColor=INK, alignment=TA_LEFT, spaceAfter=5))
styles.add(ParagraphStyle(name="BulletCN", parent=styles["BodyCN"], leftIndent=13,
                          firstLineIndent=-9, bulletIndent=1))
styles.add(ParagraphStyle(name="CalloutCN", parent=styles["BodyCN"], backColor=PALE,
                          borderColor=colors.HexColor("#D6E3EC"), borderWidth=0.6,
                          borderPadding=7, spaceBefore=5, spaceAfter=8))
styles.add(ParagraphStyle(name="CaptionCN", fontName=FONT, fontSize=8, leading=12,
                          textColor=MUTED, alignment=TA_CENTER, spaceAfter=9))

source = SOURCE.read_text()
lines = source.splitlines()
story = [Spacer(1, 22), Paragraph("整体架构设计", styles["DocTitleCN"]),
         Paragraph("Taycan Challenge · F2/FASTER 存储底座", styles["SubtitleCN"]),
         Paragraph("本文对应仓库当前 F2 候选实现。构建和全规模验收未完成的能力均明确标记为候选，不作为已通过结论。",
                   styles["CalloutCN"]), Spacer(1, 8), ArchitectureDiagram(),
         Paragraph("图 1：同步请求、F2 索引/HybridLog、持久化前沿与恢复回收的关系", styles["CaptionCN"])]

skip_mermaid = False
for line in lines:
    s = line.strip()
    if s.startswith("```mermaid"):
        skip_mermaid = True
        continue
    if skip_mermaid:
        if s == "```":
            skip_mermaid = False
        continue
    if not s or s == "---":
        story.append(Spacer(1, 3))
        continue
    if s.startswith("# "):
        continue
    if s.startswith("## "):
        story.append(Paragraph(escape(s[3:]), styles["H1CN"]))
        continue
    if s.startswith("### "):
        story.append(Paragraph(escape(s[4:]), styles["H2CN"]))
        continue
    if s.startswith("- "):
        story.append(Paragraph("• " + escape(s[2:]), styles["BulletCN"]))
        continue
    if s.startswith("```"):
        continue
    story.append(Paragraph(escape(s), styles["BodyCN"]))

doc = BaseDocTemplate(str(OUTPUT), pagesize=A4, leftMargin=44, rightMargin=44,
                      topMargin=48, bottomMargin=48, title="Taycan 整体架构设计",
                      author="Taycan Challenge")
frame = Frame(44, 48, A4[0]-88, A4[1]-96, id="normal")
doc.addPageTemplates([PageTemplate(id="main", frames=frame, onPage=page)])
doc.build(story)
print(OUTPUT)
