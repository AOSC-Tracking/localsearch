subdir('common')

subdir('libtracker-miners-common')
subdir('tracker-miner-fs')

if have_tracker_extract
  subdir('libtracker-extract')
endif

subdir('services')

test_bus_conf_file = configure_file(
  input: 'test-bus.conf.in',
  output: 'test-bus.conf',
  configuration: conf)

testdomainconf = configuration_data()
testdomainconf.set('TEST_ONTOLOGIES_DIR', tracker_uninstalled_nepomuk_ontologies_dir)
test_domain_rule = configure_file(
  input: 'test-domain.rule.in',
  output: 'test-domain.rule',
  configuration: testdomainconf
)

if get_option('functional_tests')
  subdir('functional-tests')
endif
