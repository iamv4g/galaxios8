#!/usr/bin/env python3
"""Apply the SWEET bring-up changes to an iOS root made by tools/mkiosroot.sh.

  sudo sweet-root-tweaks.py MOUNTED_ROOT

MOUNTED_ROOT is the HFS+ image mounted read-write (Linux: run
`fsck.hfsplus -fy` on the image, then `mount -t hfsplus -o loop,force`).
Run as root (it writes root/mobile owned files). Idempotent. This is what
turned the plain mkiosroot.sh output into the images that reach the home
screen (image versions v8-v14 in NOTES.md):

  launchd.plist   _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not
                  panic the system); CommCenter jobs removed (no baseband: it
                  crash-loops and Setup blocks on its CoreTelephony XPC)
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  (on iOS they live on the data volume)
  migration       LastSystemVersion so DataMigration does not run for an hour
                  on every boot
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash);
                  BTAudioHALPlugin disabled (no bluetoothd: its XPC connection's
                  error handler reconnects at once, mediaserverd ~0.5 CPU, galaxios8 c68)
  MediaToolbox    StartRecorderServer=false (camera filter prewarm calls a
                  NULL OpenGL function without a GPU -> mediaserverd crash loop)
  SpringBoard     BKNoWatchdogs (one slow CPU path, no scene-create watchdog
                  kills), no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion 11,
                  mini-buddy flags off
  activation      mobileactivationd data ark seeded; the actual Activated state
                  comes from its Hactivation branch, selected by the /chosen
                  security-status properties in tools/mkadt.py
"""
import glob, os, plistlib, shutil, sys

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist (mobile, 0600)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


# Daemons for hardware the Galaxy S8 port does not drive. galaxios8 run p5
# (14 min): locationd crashed 106 times, WirelessRadioManagerd 45 times,
# bluetoothd and fairplayd.H2 were started ~80 times each (KeepAlive), and
# ReportCrash handled every crash, all while the user was trying the UI.
REMOVED_JOBS = {
    'com.apple.CommCenter',
    'com.apple.locationd',              # GPS / sensors behind the SSP hub
    'com.apple.bluetoothd',             # BCM4361 not driven
    'com.apple.WirelessRadioManager',   # modem / radios
    'com.apple.fairplayd.H2',           # FairPlay: App Store apps only
    # run p7: started hundreds of times, each exits at once
    'com.apple.lskdd',                  # AppleKeyServices (needs the SEP)
    'com.apple.nanotimekitcompaniond',  # Apple Watch faces
    # galaxios8 c64-c67: with CommCenter gone, Visual Voicemail's CoreTelephony client
    # reconnects without pause: vmd ~1 CPU, launchd and logd another ~1.5 serving it
    'com.apple.voicemail.vmd',
}


def launchd(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    removed = [k for k, j in jobs.items() if j.get('Label') in REMOVED_JOBS]
    for k in removed:
        del jobs[k]
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, {len(removed)} jobs removed '
          f'({", ".join(sorted(jobs_label for jobs_label in REMOVED_JOBS))})')


def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            os.chown(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        print('warning: no mobileactivationd system container in the image; skipped data ark')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    os.chmod(d, 0o755)
    save(os.path.join(d, 'data_ark.plist'), {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }, MOBILE, MOBILE, 0o600)
    print('data ark', d)


def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    bt_audio(root)
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off', os.path.relpath(conf, root))


def bt_audio(root):
    src = os.path.join(root, 'System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver')
    dst_dir = os.path.join(root, 'System/Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('BTAudioHALPlugin.driver disabled')


REDUCE_EFFECTS = '--reduce-effects' in sys.argv


def main():
    args = [a for a in sys.argv[1:] if a != '--reduce-effects']
    if len(args) != 1 or not os.path.isdir(os.path.join(args[0], 'System/Library/xpc')):
        sys.exit(__doc__)
    root = args[0]
    launchd(root)
    containers(root)
    activation(root)
    audio(root)
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})
    if REDUCE_EFFECTS:
        reduce_effects(root)


def reduce_effects(root):
    # No GPU: CoreAnimation renders on the CPU, where background blurs (dock,
    # folders, Control Center, notifications) and motion effects cost the
    # most. Settings > Accessibility: Reduce Transparency, Reduce Motion.
    # Changes the look of iOS, so only with --reduce-effects.
    merge_pref(root, 'com.apple.Accessibility', {
        'EnhancedBackgroundContrastEnabled': True,
        'ReduceMotionEnabled': True})
    print('Reduce Transparency and Reduce Motion on')


GPU_TESTS = ('s8malitest', 's8gltest')


def gputests(root, outdir):
    """Install the GPU tests built on the Mac (shim/dreamlte/out: s8malitest, s8gltest, ad-hoc
    signed; research/mesa-ios/out/libOSMesa.8.dylib) into /usr/local and a launchd job per test
    that runs it once at boot. Results reach the kernel log through the S8Mali Report selector."""
    files = [(os.path.join(outdir, t), 'usr/local/bin/' + t) for t in GPU_TESTS]
    files.append((os.path.join(outdir, 'libOSMesa.8.dylib'), 'usr/local/lib/libOSMesa.8.dylib'))
    for src, rel in files:
        dst = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        os.chown(dst, 0, 0)
        os.chmod(dst, 0o755)
        print('installed', '/' + rel)
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    for t in GPU_TESTS:
        plist['LaunchDaemons'][f'/System/Library/LaunchDaemons/com.galaxios8.{t}.plist'] = {
            'Label': f'com.galaxios8.{t}',
            'ProgramArguments': [f'/usr/local/bin/{t}'],
            'RunAtLoad': True,
            'UserName': 'root',
            'StandardOutPath': f'/private/var/root/{t}.log',
            'StandardErrorPath': f'/private/var/root/{t}.log',
        }
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print('launchd jobs:', ', '.join('com.galaxios8.' + t for t in GPU_TESTS))


def drop(root, outdir):
    """Install s8drop (tools/dreamlte/s8drop.c) with a boot job, and remove the per-test jobs of
    gputests(): from now on test programs come from the drop zone on USERDATA."""
    dst = os.path.join(root, 'usr/local/bin/s8drop')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(os.path.join(outdir, 's8drop'), dst)
    os.chown(dst, 0, 0)
    os.chmod(dst, 0o755)
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    jobs = plist['LaunchDaemons']
    for t in GPU_TESTS:
        jobs.pop(f'/System/Library/LaunchDaemons/com.galaxios8.{t}.plist', None)
    jobs['/System/Library/LaunchDaemons/com.galaxios8.s8drop.plist'] = {
        'Label': 'com.galaxios8.s8drop',
        'ProgramArguments': ['/usr/local/bin/s8drop'],
        'RunAtLoad': True,
        'UserName': 'root',
        'StandardOutPath': '/private/var/root/s8drop.log',
        'StandardErrorPath': '/private/var/root/s8drop.log',
    }
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print('installed /usr/local/bin/s8drop and its boot job; GPU test jobs removed')


def ca(root, outdir):
    """Install libs8ca.dylib (tools/dreamlte/ca/s8ca.c) and insert it into backboardd: the
    CoreAnimation bridge to the Mali. It does nothing unless the kernel has boot-arg s8ca=1, and
    dyld ignores DYLD_INSERT_LIBRARIES on a kernel without the AMFI dyld policy (before 0124 of
    2026-10-24), so daily images are not affected."""
    dst = os.path.join(root, 'usr/local/lib/libs8ca.dylib')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(os.path.join(outdir, 'libs8ca.dylib'), dst)
    os.chown(dst, 0, 0)
    os.chmod(dst, 0o755)
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    jobs = [j for j in plist['LaunchDaemons'].values() if j.get('Label') == 'com.apple.backboardd']
    assert len(jobs) == 1, f'{len(jobs)} backboardd jobs'
    env = jobs[0].setdefault('EnvironmentVariables', {})
    print('backboardd environment before:', env)
    env['DYLD_INSERT_LIBRARIES'] = '/usr/local/lib/libs8ca.dylib'
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print('installed /usr/local/lib/libs8ca.dylib; backboardd gets DYLD_INSERT_LIBRARIES')


if __name__ == '__main__':
    main()
