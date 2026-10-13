import pefile,sys
pe=pefile.PE(sys.argv[1],fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE'],pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_TLS'],pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG']])
for e in getattr(pe,'DIRECTORY_ENTRY_IMPORT',[]):
    print(e.dll.decode(), len(e.imports))
    for i in e.imports:
        print('   ', hex(i.address), i.name.decode() if i.name else 'ord%d'%i.ordinal)
if hasattr(pe,'VS_VERSIONINFO'):
    for fi in pe.FileInfo:
        for x in fi:
            if hasattr(x,'StringTable'):
                for st in x.StringTable:
                    for k,v in st.entries.items(): print('VER',k.decode(),v.decode())
if hasattr(pe,'VS_FIXEDFILEINFO'):
    ff=pe.VS_FIXEDFILEINFO[0]
    print('FIXED', hex(ff.FileVersionMS), hex(ff.FileVersionLS), hex(ff.ProductVersionMS), hex(ff.ProductVersionLS))
if hasattr(pe,'DIRECTORY_ENTRY_TLS'):
    print('TLS', pe.DIRECTORY_ENTRY_TLS.struct)
