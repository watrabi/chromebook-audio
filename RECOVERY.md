# Recovering from a bad driver (no restore point needed)

The drivers in this repo are not "boot start" drivers, so **Safe Mode never loads them**.

## 1. Safe Mode (first choice)

1. If Windows fails to start twice, the blue **Recovery** screen appears automatically.
   (From a working desktop: hold **Shift** while clicking **Restart**.)
2. **Troubleshoot → Advanced options → Startup Settings → Restart**, then press **4**.
3. In Safe Mode, open PowerShell **as administrator**:

   ```powershell
   pnputil /enum-drivers            # find the oemNN.inf whose Original Name is sofapl.inf
   pnputil /delete-driver oemNN.inf /uninstall /force
   ```

   Same for `sklhdaudbus.inf` or `da7219.inf` if one of those is the problem.

## 2. Recovery Command Prompt (if Safe Mode does not work)

Recovery screen → **Troubleshoot → Advanced options → Command Prompt**:

```bat
dir C:\Windows
:: if that fails, the Windows drive has another letter here; try D:
reg load HKLM\OFF C:\Windows\System32\config\SYSTEM
reg add HKLM\OFF\ControlSet001\Services\sofapl /v Start /t REG_DWORD /d 4 /f
reg unload HKLM\OFF
```

`Start = 4` means "disabled": Windows will never load the driver. Use the service
name `sklhdaudbus` or `da7219` instead of `sofapl` to disable those.

## 3. Windows install USB (last resort)

Boot from a Windows 10 install USB → **Repair your computer** → **Troubleshoot →
Command Prompt**, then follow step 2.

## Check recovery works before testing

From an admin PowerShell: `reagentc /info` should show **Windows RE status: Enabled**.
If it says Disabled, run `reagentc /enable`.
