
gdbus_codegen = find_program('gdbus-codegen')

org_pinos_files = ['org-pinos.c', 'org-pinos.h']

gdbus_target = custom_target('org-pinos',
output : org_pinos_files,
input : 'org.pinos.xml',
command : [gdbus_codegen,
           '--interface-prefix', 'org.pinos.',
           '--generate-c-code', 'pinos/dbus/org-pinos',
           '--c-namespace', 'Pinos',
           '--c-generate-object-manager',
           '@INPUT@'],
)
