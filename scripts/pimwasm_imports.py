"""Connect Wasm imports to explicitly supplied DPU implementations.

This is a helper module called by pimwasm_compile.py, not a standalone command.
Inputs are module import descriptions, an optional --imports JSON file, and
wasm2c-generated declarations. Without a binding file, only the default
 env.abort, env.assert and env.tasklet_id functions can be resolved.

It checks import names, kinds and signatures; renames imports in the temporary
Wasm used by wasm2c; and generates imports.c plus copies of supplied native
source files. It also prepares imported memory, globals and function tables.
The original Wasm import names remain in the bundle metadata.

We need it because imported functions and objects have no implementation inside
Wasm: the embedding must provide them. Bindings are explicit; unknown imports
are rejected rather than replaced with guessed implementations.

Success means the imports have compatible bindings and their glue was generated.
It does not prove the native implementations are safe or correct. Those sources
are trusted DPU code and still need compilation and verification.
"""
import json
from pathlib import Path
import re

BUILTINS = {('env','abort'): ([],[], 'abort'),
            ('env','assert'): (['i32'],[], 'assert'),
            ('env','tasklet_id'): ([],['i32'], 'tasklet_id')}

def load(path):
    if path is None: return {'version':1,'bindings':[], 'sources':[], '_directory':None}
    path=Path(path).resolve()
    obj=json.loads(path.read_text())
    if obj.get('version')!=1 or not isinstance(obj.get('bindings',[]),list):
        raise ValueError('import bindings require version 1 and a bindings array')
    if not isinstance(obj.get('sources',[]),list): raise ValueError('native sources must be an array')
    if 'reset' in obj and not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*',obj['reset']): raise ValueError('invalid native reset symbol')
    obj['_directory']=str(path.parent)
    names=set()
    for binding in obj['bindings']:
        key=(binding.get('module'),binding.get('name'))
        if not all(isinstance(n,str) for n in key) or key in names:
            raise ValueError('duplicate or invalid import binding name')
        names.add(key)
        if binding.get('kind') not in ('function','global','memory','table'):
            raise ValueError('invalid import binding kind')
    return obj

def resolve(spec,module,name,kind,params=None,results=None):
    matches=[b for b in spec['bindings'] if (b['module'],b['name'])==(module,name)]
    if matches:
        b=dict(matches[0])
        if b['kind']!=kind: raise ValueError('incompatible import binding kind')
        if kind=='function':
            if b.get('parameters')!=params or b.get('results')!=results:
                raise ValueError('incompatible import function signature')
            if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*',b.get('symbol','')):
                raise ValueError('native import requires an explicit C symbol')
        return b
    if kind=='function' and (module,name) in BUILTINS:
        p,r,symbol=BUILTINS[module,name]
        if (params,results)!=(p,r): raise ValueError('incompatible builtin signature')
        return {'kind':kind,'builtin':symbol}
    raise ValueError(f'unresolved import {module!r}.{name!r}; supply --imports bindings.json')

def generation(data,info):
    """Rename only imports in a generator copy; original bytes remain intact."""
    from pimwasm_compile import Reader, require, encode_uleb
    def name(s):
        b=s.encode();return encode_uleb(len(b))+b
    r=Reader(data);out=bytearray(r.take(8)); modules={}
    for record in info['imports']:
        modules.setdefault(record['module'],f'pimwasmimp{len(modules):02}')
        record['c_module']=modules[record['module']]
        record['c_name']=f'item{record["ordinal"]}'
    info['import_modules']=list(modules.values())
    while r.pos<len(data):
        start=r.pos;kind=r.byte();payload=r.take(r.uint())
        if kind!=2: out.extend(data[start:r.pos]);continue
        s=Reader(payload);count=s.uint();buf=bytearray(encode_uleb(count))
        require(count==len(info['imports']),'import mapping count changed')
        for record in info['imports']:
            require((s.name(),s.name())==(record['module'],record['name']),'import mapping order changed')
            begin=s.pos;external=s.byte()
            if external==0: s.uint()
            elif external==3: s.byte();s.byte()
            else:
                if external==1: s.byte()
                flags=s.uint();s.uint()
                if flags&1: s.uint()
            buf.extend(name(record['c_module'])+name(record['c_name'])+s.data[begin:s.pos])
        s.done();out.extend(bytes([kind])+encode_uleb(len(buf))+buf)
    return bytes(out)

def validate_header(header,info):
    from pimwasm_compile import require,C_TYPES,c_result,MODULE_TYPE
    expected=[(b['c_module'],b['c_name']) for b in info['imports']]
    require(sorted(re.findall(r"/\* import: '([^']+)' '([^']+)' \*/",header))==sorted(expected),'unexpected generated import names')
    for b in info['imports']:
        context='struct w2c_'+b['c_module']+'*'
        symbol='w2c_'+b['c_module']+'_'+b['c_name']
        if b['kind']=='function':
            decl=c_result(b['result_types'])+' '+symbol+'('+context+''.join(', '+C_TYPES[t] for t in b['parameter_types'])+');'
        else:
            typ={'global':C_TYPES.get(b.get('type'),'void'),'memory':'wasm_rt_memory_t','table':'wasm_rt_funcref_table_t'}[b['kind']]
            decl='extern '+typ+'* '+symbol+'('+context+');'
        require(decl in header,'unexpected generated import declaration: '+symbol)
    params=''.join(', struct w2c_'+m+'*' for m in info['import_modules'])
    require('void wasm2c_pimwasm__module_instantiate('+MODULE_TYPE+'*'+params+');' in header,
            'unexpected generated instantiate imports')

def write(stage,info,spec):
    from pimwasm_compile import C_TYPES,c_result,require
    # Contexts carry only native object state. Guest pointers cannot expose them.
    text='#include "module.h"\n#include "wasm-rt-memory.h"\n#include <string.h>\n'
    for module in info['import_modules']:
        text+=f'struct w2c_{module} {{ unsigned reserved; }};\nstatic struct w2c_{module} context_{module};\n'
    reset=''; declarations='';global_objects={}
    if spec.get('reset'):
        declarations+='void '+spec['reset']+'(void);\n'
        reset+=spec['reset']+'();\n'
    for b in info['imports']:
        ctx='struct w2c_'+b['c_module']+'*'; symbol='w2c_'+b['c_module']+'_'+b['c_name']; binding=b['binding'];idx=b['ordinal']
        if b['kind']=='function':
            args=''.join(f', {C_TYPES[t]} a{i}' for i,t in enumerate(b['parameter_types']))
            text+=c_result(b['result_types'])+' '+symbol+'('+ctx+' context'+args+') {\n (void)context;\n'
            actual=', '.join(f'a{i}' for i in range(len(b['parameter_types'])))
            if 'builtin' in binding:
                target='pimwasm_builtin_'+binding['builtin']
            else: target=binding['symbol']
            signature=c_result(b['result_types'])+' '+target+'('+(', '.join(C_TYPES[t] for t in b['parameter_types']) or 'void')+');\n'
            declarations+=signature
            text+=(' '+('return ' if b['result_types'] else '')+target+'('+actual+');\n}\n')
        elif b['kind']=='global':
            key=(b['module'],b['name']);canonical=global_objects.setdefault(key,idx)
            typ=C_TYPES[b['type']]
            if canonical!=idx:
                text+=f'{typ}* {symbol}({ctx} context) {{ (void)context;return &imported_global_{canonical}; }}\n';continue
            text+=f'static {typ} imported_global_{idx};\n{typ}* {symbol}({ctx} context) {{ (void)context;return &imported_global_{idx}; }}\n'
            if b['type'] in ('f32','f64'):
                bits='uint32_t' if b['type']=='f32' else 'uint64_t';size=4 if b['type']=='f32' else 8
                reset+=f' {{ {bits} bits=UINT64_C({b["bits"]}); memcpy(&imported_global_{idx},&bits,{size}); }}\n'
            else: reset+=f' imported_global_{idx}=UINT64_C({b["bits"]});\n'
        elif b['kind']=='memory':
            text+=f'static wasm_rt_memory_t imported_memory;\nwasm_rt_memory_t* {symbol}({ctx} context) {{ (void)context;return &imported_memory; }}\n'
            reset+=f' wasm_rt_allocate_memory(&imported_memory,{binding["initial"]},{binding["maximum"]},false);\n'
            for n,data in enumerate(binding.get('data',[])):
                offset=data.get('offset');raw=bytes.fromhex(data.get('hex',''))
                require(type(offset)is int and 0<=offset<=binding['initial']*65536 and len(raw)<=binding['initial']*65536-offset,
                        'import memory initializer exceeds current size')
                if raw:
                    text+=f'static const uint8_t import_data_{n}[]={{'+','.join(str(x) for x in raw)+'};\n'
                    reset+=f' pimwasm_load_data(&imported_memory,{offset},import_data_{n},{len(raw)});\n'
        else:
            text+=f'static wasm_rt_funcref_table_t imported_table;\nwasm_rt_funcref_table_t* {symbol}({ctx} context) {{ (void)context;return &imported_table; }}\n'
            reset+=f' wasm_rt_allocate_funcref_table(&imported_table,{binding["initial"]},{binding["maximum"]});\n'
            declarations+='void pimwasm_guest_import_table_entries(wasm_rt_funcref_table_t*,void**);\n'
            reset+=' { void *contexts[]={'+','.join('&context_'+m for m in info['import_modules'])+'}; pimwasm_guest_import_table_entries(&imported_table,contexts); }\n'
    # Put native prototypes ahead of wrappers, preserving ordinary C type checks.
    text=text.replace('#include <string.h>\n','#include <string.h>\n'+declarations,1)
    text+='static void pimwasm_imports_reset(void) {\n'+reset+'}\n'
    for i,path in enumerate(spec.get('sources',[])):
        require(isinstance(path,str) and spec['_directory'] is not None,'invalid native binding source')
        src=(Path(spec['_directory'])/path).resolve()
        require(src.is_file(),'missing native import implementation '+str(src))
        filename=f'binding_{i}.c';(stage/filename).write_bytes(src.read_bytes())
        text+=f'#include "{filename}"\n'
    (stage/'imports.c').write_text(text)
    return ''.join(', &context_'+m for m in info['import_modules'])
