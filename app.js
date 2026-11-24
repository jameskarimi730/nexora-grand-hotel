document.addEventListener("DOMContentLoaded", () => {
  document.querySelectorAll("[data-year]").forEach(el => el.textContent = new Date().getFullYear());
  const apiBase = (window.API_BASE || "").replace(/\/$/, "");
  const roomPrices = { standard: 3500, deluxe: 5500, executive: 8500 };
  const roomNames = { standard: "Standard Room", deluxe: "Deluxe Room", executive: "Executive Suite" };
  const today = new Date().toISOString().slice(0, 10);
  const checkIn = document.getElementById("checkIn");
  const checkOut = document.getElementById("checkOut");
  if (checkIn) checkIn.min = today;
  if (checkOut) checkOut.min = today;

  const roomSelect = document.getElementById("roomType");
  if (roomSelect) {
    const requested = new URLSearchParams(location.search).get("room");
    if (requested && roomPrices[requested]) roomSelect.value = requested;
  }
  function nightsBetween(a, b) {
    if (!a || !b) return 0;
    const start = new Date(a + "T00:00:00Z");
    const end = new Date(b + "T00:00:00Z");
    return Math.round((end - start) / 86400000);
  }
  function updateEstimate() {
    const room = roomSelect?.value;
    const nights = nightsBetween(checkIn?.value, checkOut?.value);
    const target = document.getElementById("pricePreview");
    if (!target) return;
    if (room && nights > 0) target.textContent = `${nights} night(s) × KSh ${roomPrices[room].toLocaleString()} = KSh ${(nights * roomPrices[room]).toLocaleString()}`;
    else target.textContent = "Select a room and valid dates to estimate your stay.";
    if (checkOut && checkIn?.value) checkOut.min = checkIn.value;
  }
  [roomSelect, checkIn, checkOut].forEach(el => el?.addEventListener("change", updateEstimate));
  updateEstimate();

  const bookingForm = document.getElementById("booking-form");
  bookingForm?.addEventListener("submit", async e => {
    e.preventDefault();
    const message = document.getElementById("bookingMessage");
    const data = Object.fromEntries(new FormData(bookingForm).entries());
    const nights = nightsBetween(data.checkIn, data.checkOut);
    if (!apiBase || apiBase.includes("YOUR-C-BACKEND")) {
      message.className = "message error";
      message.textContent = "The booking service has not been connected yet. Deploy the C backend and update config.js first.";
      return;
    }
    if (nights < 1) {
      message.className = "message error";
      message.textContent = "Please choose a check-out date after your check-in date.";
      return;
    }
    const button = bookingForm.querySelector("button[type=submit]");
    button.disabled = true; button.textContent = "Checking availability...";
    message.className = "message"; message.textContent = "";
    try {
      const response = await fetch(`${apiBase}/api/bookings`, {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(data)
      });
      const result = await response.json();
      if (!response.ok || !result.success) throw new Error(result.error || "Unable to create reservation.");
      message.className = "message success";
      message.innerHTML = `Your reservation has been created.<br><strong>Booking reference: ${escapeHtml(result.bookingCode)}</strong><br>${escapeHtml(result.roomName)} · ${result.nights} night(s) · KSh ${Number(result.totalAmount).toLocaleString()}<br>Keep this reference to look up your reservation.`;
      bookingForm.reset(); updateEstimate();
    } catch (err) {
      message.className = "message error"; message.textContent = err.message || "Could not reach the booking service.";
    } finally { button.disabled = false; button.textContent = "Check availability & book →"; }
  });

  const lookupForm = document.getElementById("lookup-form");
  lookupForm?.addEventListener("submit", async e => {
    e.preventDefault();
    const message = document.getElementById("lookupMessage");
    const resultBox = document.getElementById("reservationResult");
    const code = new FormData(lookupForm).get("bookingCode").trim().toUpperCase();
    if (!apiBase || apiBase.includes("YOUR-C-BACKEND")) {
      message.className = "message error"; message.textContent = "The booking service has not been connected yet. Deploy the C backend and update config.js first."; return;
    }
    message.className = "message"; message.textContent = "Searching...";
    resultBox.innerHTML = "";
    try {
      const response = await fetch(`${apiBase}/api/bookings?code=${encodeURIComponent(code)}`);
      const data = await response.json();
      if (!response.ok || !data.success) throw new Error(data.error || "Reservation not found.");
      const b = data.booking;
      resultBox.innerHTML = `<div class="reservation-card"><p class="eyebrow">RESERVATION DETAILS</p><h2>${escapeHtml(b.bookingCode)}</h2><div class="reservation-grid"><span>Guest</span><b>${escapeHtml(b.guestName)}</b><span>Room</span><b>${escapeHtml(b.roomName)}</b><span>Check-in</span><b>${escapeHtml(b.checkIn)}</b><span>Check-out</span><b>${escapeHtml(b.checkOut)}</b><span>Nights</span><b>${b.nights}</b><span>Total</span><b>KSh ${Number(b.totalAmount).toLocaleString()}</b><span>Status</span><b>${escapeHtml(b.status)}</b></div></div>`;
      message.textContent = "";
    } catch (err) { message.className = "message error"; message.textContent = err.message; }
  });
});
function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, ch => ({ "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;" }[ch]));
}
