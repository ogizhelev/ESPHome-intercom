# ESPHome codegen for the `intercom` external component.
#
# ESPHome's loader reads CODEOWNERS / AUTO_LOAD / CONFIG_SCHEMA and `async def to_code(config)`
# as module-level names. The config vocabulary + schema are encapsulated in IntercomConfig and
# the imperative build logic in IntercomCodegen; the module-level names are thin assignments
# from those classes.

from typing import ClassVar

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, button, number, sensor, text_sensor
from esphome.components import select as select_
from esphome.components import switch as switch_
from esphome.components import time as time_
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID

intercom_ns = cg.esphome_ns.namespace("intercom")
IntercomReceiver = intercom_ns.class_("IntercomReceiver", cg.Component)
IntercomNumber = intercom_ns.class_("IntercomNumber", number.Number)
IntercomSwitch = intercom_ns.class_("IntercomSwitch", switch_.Switch)
IntercomSelect = intercom_ns.class_("IntercomSelect", select_.Select)
OpenDoorButton = intercom_ns.class_("OpenDoorButton", button.Button)
PickupButton = intercom_ns.class_("PickupButton", button.Button)
HangupButton = intercom_ns.class_("HangupButton", button.Button)

# 3-way mode select options (index 0/1/2) shared by both features.
MODE_OPTIONS = ["Off", "On", "Schedule"]
MODE_YAML = {"off": 0, "on": 1, "schedule": 2}


class IntercomConfig:
    """Config vocabulary and schema for the intercom component.

    Config keys are class attributes and the schemas are built by classmethods. ESPHome's
    loader looks up CODEOWNERS / AUTO_LOAD / CONFIG_SCHEMA as module-level names, so those are
    assigned from this class just below it.
    """

    CODEOWNERS: ClassVar[list[str]] = ["@ogizhelev"]
    AUTO_LOAD: ClassVar[list[str]] = [
        "binary_sensor", "sensor", "text_sensor", "number", "switch", "select", "button",
    ]

    BUS_PIN = "bus_pin"
    RING_PIN = "ring_pin"
    OFFHOOK_PIN = "offhook_pin"
    MUTE_PIN = "mute_pin"
    TIME_ID = "time_id"
    OFFHOOK_INVERTED = "offhook_inverted"
    MUTE_INVERTED = "mute_inverted"
    RING_INVERTED = "ring_inverted"
    DUMP_RAW = "dump_raw"
    FRAME_GAP = "frame_gap"
    RING_MIN_HOLD = "ring_min_hold"
    OPEN_DELAY = "open_delay"
    CLEAR_ON_OPEN = "clear_on_open"
    AUTO_OPEN = "auto_open"
    MUTE = "mute"

    # Ordered Mon..Sun (index 0..6), shared by the schemas and the day-switch builder.
    DAYS: ClassVar[list[str]] = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
    DAY_LABEL: ClassVar[dict[str, str]] = {"mon": "Mon", "tue": "Tue", "wed": "Wed", "thu": "Thu", "fri": "Fri", "sat": "Sat", "sun": "Sun"}

    @classmethod
    def _days(cls):
        return cv.ensure_list(cv.one_of(*cls.DAYS, lower=True))

    @classmethod
    def auto_open_schema(cls):
        return cv.Schema(
            {
                cv.Optional("mode", default="schedule"): cv.one_of(*MODE_YAML, lower=True),
                cv.Optional("on_ring", default=True): cv.boolean,
                cv.Optional("open_apartment", default=0): cv.int_range(min=0, max=255),
                cv.Optional("from_hour", default=8): cv.int_range(min=0, max=23),
                cv.Optional("until_hour", default=18): cv.int_range(min=0, max=23),
                cv.Optional("days", default=["mon", "tue", "wed", "thu", "fri"]): cls._days(),
            }
        )

    @classmethod
    def mute_schema(cls):
        return cv.Schema(
            {
                cv.Optional("mode", default="schedule"): cv.one_of(*MODE_YAML, lower=True),
                cv.Optional("from_hour", default=22): cv.int_range(min=0, max=23),
                cv.Optional("until_hour", default=7): cv.int_range(min=0, max=23),
                cv.Optional("days", default=["mon", "tue", "wed", "thu", "fri", "sat", "sun"]): cls._days(),
            }
        )

    @classmethod
    def schema(cls):
        return cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(IntercomReceiver),
                cv.Required(cls.BUS_PIN): pins.internal_gpio_input_pin_schema,
                cv.Optional(cls.RING_PIN): pins.internal_gpio_input_pin_schema,
                cv.Required(cls.OFFHOOK_PIN): pins.internal_gpio_output_pin_schema,
                cv.Optional(cls.MUTE_PIN): pins.internal_gpio_output_pin_schema,
                cv.Required(cls.TIME_ID): cv.use_id(time_.RealTimeClock),
                cv.Optional(cls.OFFHOOK_INVERTED, default=False): cv.boolean,
                cv.Optional(cls.MUTE_INVERTED, default=False): cv.boolean,
                cv.Optional(cls.RING_INVERTED, default=False): cv.boolean,
                cv.Optional(cls.DUMP_RAW, default=False): cv.boolean,
                cv.Optional(cls.FRAME_GAP, default="120ms"): cv.positive_time_period_milliseconds,
                cv.Optional(cls.RING_MIN_HOLD, default="120ms"): cv.positive_time_period_milliseconds,
                cv.Optional(cls.OPEN_DELAY, default="2s"): cv.positive_time_period_milliseconds,
                cv.Optional(cls.CLEAR_ON_OPEN, default=True): cv.boolean,
                cv.Optional(cls.AUTO_OPEN, default={}): cls.auto_open_schema(),
                cv.Optional(cls.MUTE, default={}): cls.mute_schema(),
            }
        ).extend(cv.COMPONENT_SCHEMA)


# ESPHome loader reads these as module-level names.
CODEOWNERS = IntercomConfig.CODEOWNERS
AUTO_LOAD = IntercomConfig.AUTO_LOAD
CONFIG_SCHEMA = IntercomConfig.schema()


class IntercomCodegen:
    """Builds the intercom component and all of its auto-created Home Assistant entities.

    One instance per `to_code` call. The component pointer (`var`) and its ID string (`base`)
    are constructed in __init__ and reused by the entity-factory methods so every generated
    entity ID is `<base>_<suffix>`.
    """

    def __init__(self, config):
        self.config = config
        # Fully construct the component here so `var`/`base` are always valid (no Optional).
        self.var = cg.new_Pvariable(config[CONF_ID])
        self.base = config[CONF_ID].id

    async def build(self):
        await cg.register_component(self.var, self.config)
        await self._apply_pins_and_options()
        await self._create_status_entities()
        await self._create_auto_open_entities()
        await self._create_mute_entities()

    # -- entity factories (auto-created, default names; IDs are <base>_<suffix>) --
    async def _make_bsensor(self, suffix, name, device_class=None, icon=None, diagnostic=True):
        kwargs = {}
        if diagnostic:
            kwargs["entity_category"] = ENTITY_CATEGORY_DIAGNOSTIC
        if device_class is not None:
            kwargs["device_class"] = device_class
        if icon is not None:
            kwargs["icon"] = icon
        bid = ID(f"{self.base}_{suffix}", is_declaration=True, type=binary_sensor.BinarySensor)
        conf = binary_sensor.binary_sensor_schema(**kwargs)({CONF_ID: bid, CONF_NAME: name})
        return await binary_sensor.new_binary_sensor(conf)

    async def _make_number(self, suffix, name, default_val, lo, hi, icon=None):
        kwargs = {"entity_category": ENTITY_CATEGORY_CONFIG}
        if icon is not None:
            kwargs["icon"] = icon
        nid = ID(f"{self.base}_{suffix}", is_declaration=True, type=IntercomNumber)
        conf = number.number_schema(IntercomNumber, **kwargs)({CONF_ID: nid, CONF_NAME: name})
        n = await number.new_number(conf, min_value=lo, max_value=hi, step=1)
        cg.add(n.set_default_value(default_val))
        return n

    async def _make_switch(self, suffix, name, default_on):
        sid = ID(f"{self.base}_{suffix}", is_declaration=True, type=IntercomSwitch)
        conf = switch_.switch_schema(
            IntercomSwitch, default_restore_mode="DISABLED", entity_category=ENTITY_CATEGORY_CONFIG
        )({CONF_ID: sid, CONF_NAME: name})
        s = await switch_.new_switch(conf)
        cg.add(s.set_default_value(default_on))
        return s

    async def _make_select(self, suffix, name, default_index):
        sid = ID(f"{self.base}_{suffix}", is_declaration=True, type=IntercomSelect)
        conf = select_.select_schema(IntercomSelect, entity_category=ENTITY_CATEGORY_CONFIG)(
            {CONF_ID: sid, CONF_NAME: name}
        )
        sel = await select_.new_select(conf, options=MODE_OPTIONS)
        cg.add(sel.set_default_index(default_index))
        return sel

    async def _make_text(self, suffix, name, icon=None):
        kwargs = {}
        if icon is not None:
            kwargs["icon"] = icon
        tid = ID(f"{self.base}_{suffix}", is_declaration=True, type=text_sensor.TextSensor)
        conf = text_sensor.text_sensor_schema(**kwargs)({CONF_ID: tid, CONF_NAME: name})
        return await text_sensor.new_text_sensor(conf)

    # -- build steps --
    async def _apply_pins_and_options(self):
        config, var, c = self.config, self.var, IntercomConfig
        cg.add(var.set_bus_pin(await cg.gpio_pin_expression(config[c.BUS_PIN])))
        cg.add(var.set_offhook_pin(await cg.gpio_pin_expression(config[c.OFFHOOK_PIN])))
        if c.MUTE_PIN in config:
            cg.add(var.set_mute_pin(await cg.gpio_pin_expression(config[c.MUTE_PIN])))
        if c.RING_PIN in config:
            cg.add(var.set_ring_pin(await cg.gpio_pin_expression(config[c.RING_PIN])))
        cg.add(var.set_time(await cg.get_variable(config[c.TIME_ID])))
        cg.add(var.set_offhook_inverted(config[c.OFFHOOK_INVERTED]))
        cg.add(var.set_mute_inverted(config[c.MUTE_INVERTED]))
        cg.add(var.set_ring_inverted(config[c.RING_INVERTED]))
        cg.add(var.set_dump_raw(config[c.DUMP_RAW]))
        cg.add(var.set_frame_gap(config[c.FRAME_GAP].total_milliseconds))
        cg.add(var.set_ring_min_hold(config[c.RING_MIN_HOLD].total_milliseconds))
        cg.add(var.set_open_delay(config[c.OPEN_DELAY].total_milliseconds))
        cg.add(var.set_clear_on_open(config[c.CLEAR_ON_OPEN]))

    async def _create_status_entities(self):
        var = self.var
        ring_bs = await self._make_bsensor("ring", "Doorbell (RING)", device_class="sound", icon="mdi:phone-ring-outline")
        cg.add(var.set_ring_sensor(ring_bs))
        dok = await self._make_bsensor("decoding_ok", "Decoding successful", icon="mdi:check")
        cg.add(var.set_decoding_successful(dok))
        owc = await self._make_bsensor("open_code", "Open with code", icon="mdi:door-open")
        cg.add(var.set_open_with_code(owc))

        dec_id = ID(f"{self.base}_decoder", is_declaration=True, type=sensor.Sensor)
        dec_conf = sensor.sensor_schema(accuracy_decimals=0, icon="mdi:counter", entity_category=ENTITY_CATEGORY_DIAGNOSTIC)({CONF_ID: dec_id, CONF_NAME: "Decoder"})
        cg.add(var.set_decoder_sensor(await sensor.new_sensor(dec_conf)))

        btn_id = ID(f"{self.base}_open_door", is_declaration=True, type=OpenDoorButton)
        btn_conf = button.button_schema(OpenDoorButton, icon="mdi:door")({CONF_ID: btn_id, CONF_NAME: "Open door"})
        btn = await button.new_button(btn_conf)
        cg.add(btn.set_parent(var))

        pickup_id = ID(f"{self.base}_pickup", is_declaration=True, type=PickupButton)
        pickup_conf = button.button_schema(PickupButton, icon="mdi:phone-in-talk")({CONF_ID: pickup_id, CONF_NAME: "Pickup"})
        pickup_btn = await button.new_button(pickup_conf)
        cg.add(pickup_btn.set_parent(var))

        hangup_id = ID(f"{self.base}_hangup", is_declaration=True, type=HangupButton)
        hangup_conf = button.button_schema(HangupButton, icon="mdi:phone-hangup")({CONF_ID: hangup_id, CONF_NAME: "Hang up"})
        hangup_btn = await button.new_button(hangup_conf)
        cg.add(hangup_btn.set_parent(var))

    async def _create_auto_open_entities(self):
        var = self.var
        ao = self.config[IntercomConfig.AUTO_OPEN]
        cg.add(var.set_auto_mode(await self._make_select("ao_mode", "Auto open", MODE_YAML[ao["mode"]])))
        cg.add(var.set_open_apartment(await self._make_number("open_apartment", "Open apartment", ao["open_apartment"], 0, 255, icon="mdi:numeric")))
        cg.add(var.set_auto_on_ring(await self._make_switch("ao_on_ring", "Auto open on RING", ao["on_ring"])))
        cg.add(var.set_auto_from(await self._make_number("ao_from", "Auto open from hour", ao["from_hour"], 0, 23, icon="mdi:clock-start")))
        cg.add(var.set_auto_until(await self._make_number("ao_until", "Auto open until hour", ao["until_hour"], 0, 23, icon="mdi:clock-end")))
        for i, d in enumerate(IntercomConfig.DAYS):
            sw = await self._make_switch(f"ao_day_{d}", f"Auto open {IntercomConfig.DAY_LABEL[d]}", d in ao["days"])
            cg.add(var.set_auto_day(i, sw))
        cg.add(var.set_auto_active(await self._make_bsensor("auto_active", "Auto open active", icon="mdi:door-open", diagnostic=False)))
        cg.add(var.set_auto_source(await self._make_text("auto_source", "Auto open source", icon="mdi:information-outline")))

    async def _create_mute_entities(self):
        var = self.var
        mu = self.config[IntercomConfig.MUTE]
        cg.add(var.set_mute_mode(await self._make_select("mute_mode", "Mute", MODE_YAML[mu["mode"]])))
        cg.add(var.set_mute_from(await self._make_number("mute_from", "Mute from hour", mu["from_hour"], 0, 23, icon="mdi:clock-start")))
        cg.add(var.set_mute_until(await self._make_number("mute_until", "Mute until hour", mu["until_hour"], 0, 23, icon="mdi:clock-end")))
        for i, d in enumerate(IntercomConfig.DAYS):
            sw = await self._make_switch(f"mute_day_{d}", f"Mute {IntercomConfig.DAY_LABEL[d]}", d in mu["days"])
            cg.add(var.set_mute_day(i, sw))
        cg.add(var.set_mute_active(await self._make_bsensor("mute_active", "Mute active", icon="mdi:volume-off", diagnostic=False)))
        cg.add(var.set_mute_source(await self._make_text("mute_source", "Mute source", icon="mdi:information-outline")))


async def to_code(config):
    await IntercomCodegen(config).build()
