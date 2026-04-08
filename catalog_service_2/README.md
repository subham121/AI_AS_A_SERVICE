# catalog_service_2

Standalone manifest-driven pack catalog service.

## Run

```bash
python3 app.py --host 10.221.31.77 --port 5000 --advertise-host 10.221.31.77
```

## APIs

- `GET /apiv1/getCapabilityList`
- `GET /apiv1/getCompatiblePackList?capability=<slug>&device_capability=<json>`
- `GET /apiv1/getPackDetails?pack_id=<pack_id>`
- `GET /bundles/<bundle_file>`
- `GET /healthz`
