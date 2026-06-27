export function displayNameFor(device) {
  if (!device) return null;
  const name = device.display_name;
  return name && name.length > 0 ? name : device.device_id;
}
