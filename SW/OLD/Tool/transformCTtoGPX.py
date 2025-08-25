import re
import sys
def transform_line(line):
    # Utiliser une expression régulière pour extraire les valeurs
    match = re.match(r'<ti=([^,]+),la=([^;]+);lo=([^,]+),al=([^,]+),fi=([^,]+),te=([^,]+),hu=([^>]+)>', line)
    if not match:
        raise ValueError(f"Format de ligne invalide: {line}")

    A, B, C, D, E, F, G = match.groups()

    # Construire le format XML souhaité
    xml_format = f"""
<trkpt lat="{B}" lon="{C}">
  <ele>{D}</ele>
  <time>{A}</time>
  <fix>{E}</fix>
  <extension>
   <ns3:TrackPointExtension>
    <ns3:atemp>{F}</ns3:atemp>
   </ns3:TrackPointExtension>
  </extension>
</trkpt>"""

    return xml_format.strip()

def transform_file(input_file, output_file):
    with open(input_file, 'r') as infile, open(output_file, 'w') as outfile:
        outfile.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"Pacemk\">\n<name>test</name>\n<trk>\n<name>test</name>\n<number>1</number>\n<trkseg>\n")
        for line in infile:
            try:
                xml_line = transform_line(line.strip())
                outfile.write(xml_line + '\n')
            except ValueError as e:
                print(e)
                continue
        outfile.write("</trkseg>\n</trk>\n</gpx>")
# Exemple d'utilisation
input_file = sys.argv[1]  # Remplacez par le chemin de votre fichier d'entrée
output_file = sys.argv[2]   # Remplacez par le chemin de votre fichier de sortie
transform_file(input_file, output_file)
