import docx

doc = docx.Document('02-嵌入式固件架构设计说明书.docx')
with open('docx_structure.txt', 'w', encoding='utf-8') as f:
    for i, p in enumerate(doc.paragraphs[:15]):
        text = p.text
        xml = p._element.xml
        has_drawing = '<w:drawing' in xml
        has_pict = '<w:pict' in xml
        f.write(f'Para {i}:\n')
        f.write(f'Text: {text}\n')
        f.write(f'Has Drawing: {has_drawing}\n')
        f.write(f'Has Pict: {has_pict}\n')
        f.write('-'*20 + '\n')
