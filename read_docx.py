import docx

doc = docx.Document('02-嵌入式固件架构设计说明书.docx')
with open('docx_content.txt', 'w', encoding='utf-8') as f:
    for i, p in enumerate(doc.paragraphs[:30]):
        f.write(f'{i}: {p.text}\n')
