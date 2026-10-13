# Verification

This page groups the verification procedures for the device. The host and QEMU checks are listed in
the [README](../README.md#check-it-yourself); the process for rebuilding the pinned artifacts is in
[Reproducible builds](reproducible-build.md).

## Browser demo signet transaction

The signet build's `send.html` embeds the browser signer and runs the transaction flow on one page.
It uses the built-in public BIP39 test seed, so only use valueless signet coins. Never load a real
seed or use coins with value.

1. Build and serve the page:

   ```sh
   make web
   cd build/web-signet
   python3 -m http.server 8000
   ```

   Open `http://localhost:8000/send.html`. On the embedded signer, choose **Scan SeedQR** and scan
   the public test SeedQR displayed in step 1. The page derives its first BIP84
   receive address after the signer loads the seed.

2. Fund the displayed address with signet test coins and wait for a confirmed UTXO. The recipient
   starts with a fixed testnet address and can be changed. The page uses mempool.space's signet API
   to look up the confirmed UTXO.

3. Set the amount and fee rate, then choose **PSBTを作成して表示**. The wallet window creates an
   unsigned one-input P2WPKH PSBT and displays its animated QR. Drag the title bar to move the
   window, then choose **Scan PSBT** on the signer. Review the output and fee, then approve there.

4. Review the signed transaction summary and choose **signetへブロードキャスト**. The page
   finalizes the PSBT in the browser and submits the raw transaction to the signet API; on success it
   shows the mempool.space transaction link. This walkthrough supports one confirmed input.
