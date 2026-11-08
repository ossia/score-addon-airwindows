// Airwindows process controls: construction from the derived metadata, registry-wide
// saved-session round-trips, and conversion of documents written by format 1.
//
// Format 1 gave every parameter a FloatSlider over [0;1] holding the raw normalized
// value. Format 2 gives it the widget and the range the metadata describes, with the
// value in display units. ProcessModel::migrateControls() reconciles whatever came out
// of the file against the metadata: a format-1 control is converted, a current one is
// left alone, and - crucially - a format-2 control whose metadata has since moved
// keeps its display value instead of being re-read as if it were normalized.

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortSerialization.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Process/Process.hpp>
#include <Process/TimeValue.hpp>
#include <Process/TimeValueSerialization.hpp>

#include <score/application/ApplicationContext.hpp>
#include <score/model/EntitySerialization.hpp>
#include <score/model/path/PathSerialization.hpp>
#include <score/plugins/SerializableHelpers.hpp>
#include <score/plugins/settingsdelegate/SettingsDelegateModel.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>
#include <score/serialization/VisitorCommon.hpp>
#include <score/tools/SafeCast.hpp>

#include <core/application/ApplicationInterface.hpp>
#include <core/application/ApplicationSettings.hpp>
#include <core/presenter/DocumentManager.hpp>

#include <ossia/math/safe_math.hpp>
#include <ossia/network/domain/domain.hpp>
#include <ossia/network/value/value_conversion.hpp>

#include <Airwindows/Metadata.hpp>
#include <Airwindows/ProcessModel.hpp>
#include <Airwindows/Registry.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <algorithm>

using Catch::Approx;

namespace
{
//! Minimal application: the port factories are all the deserializer needs.
struct TestApplication final : public score::ApplicationInterface
{
  score::ApplicationSettings appSettings;
  score::ApplicationComponentsData compData;
  score::ApplicationComponents comps{compData};
  score::DocumentList docList;
  std::vector<std::unique_ptr<score::SettingsDelegateModel>> settingsVec;
  score::ApplicationContext ctx{appSettings, comps, docList, settingsVec};

  TestApplication()
  {
    appSettings.gui = false;
    m_instance = this;

    auto pl = std::make_unique<Process::PortFactoryList>();
    pl->insert(std::make_unique<Process::PortFactory_T<Process::ControlInlet>>());
    pl->insert(std::make_unique<Process::PortFactory_T<Process::AudioInlet>>());
    pl->insert(std::make_unique<Process::PortFactory_T<Process::AudioOutlet>>());
    pl->insert(std::make_unique<Process::PortFactory_T<Process::FloatSlider>>());
    pl->insert(std::make_unique<Process::PortFactory_T<Process::LogFloatSlider>>());
    pl->insert(std::make_unique<Process::PortFactory_T<Process::IntSlider>>());
    pl->insert(std::make_unique<Process::PortFactory_T<Process::ComboBox>>());
    compData.factories.insert(
        {Process::PortFactory::static_interfaceKey(), std::move(pl)});
  }

  const score::ApplicationContext& context() const override { return ctx; }
  const score::ApplicationComponents& components() const override { return comps; }
};

static TestApplication g_app;

using Model = Airwindows::ProcessModel;

Model* makeProcess(const char* plugin, QObject& owner)
{
  return new Model{
      TimeVal::fromMsecs(1000), QString::fromLatin1(plugin),
      Id<Process::ProcessModel>{0}, &owner};
}

int controlCount(const Model& proc)
{
  return (int)proc.inlets().size() - 1; // inlet 0 is the audio input
}

rapidjson::Document toJson(const Model& proc)
{
  const auto reader
      = score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(proc));
  rapidjson::Document doc = readJson(reader.toByteArray());
  REQUIRE(!doc.HasParseError());
  return doc;
}

Model* fromJson(const rapidjson::Document& doc, QObject& owner)
{
  JSONObject::Deserializer des{doc};
  return new Model{des, &owner};
}

Model* dataStreamRoundTrip(const Model& proc, QObject& owner)
{
  const QByteArray bytes
      = score::marshall<DataStream>(static_cast<const Process::ProcessModel&>(proc));
  DataStream::Deserializer des{bytes};

  // Peel the polymorphic envelope the same way the document loader does.
  QByteArray nested;
  des.stream() >> nested;
  DataStream::Deserializer sub{nested};
  SCORE_DEBUG_CHECK_DELIMITER2(sub);
  UuidKey<Process::ProcessModel> key;
  TSerializer<DataStream, UuidKey<Process::ProcessModel>>::writeTo(sub, key);
  SCORE_DEBUG_CHECK_DELIMITER2(sub);
  REQUIRE(key == Metadata<ConcreteKey_k, Model>::get());

  return new Model{sub, &owner};
}

//! Serialize a standalone port the way the process serializes the ones it owns.
void spliceInlet(
    rapidjson::Document& doc, rapidjson::SizeType at, const Process::Inlet& port)
{
  const auto reader = score::marshall<JSONObject>(port);
  rapidjson::Document sub = readJson(reader.toByteArray());
  REQUIRE(!sub.HasParseError());
  doc["Inlets"][at].CopyFrom(sub, doc.GetAllocator());
}

/**
 * @brief Downgrade a serialized process to what format 1 actually wrote.
 *
 * Not a FloatSlider built through the range constructor - that one sets Init and
 * Hidden, which format 1 never did. The old code built the inherited ControlInlet
 * and then applied a domain, so the port carries the bare parameter name, no Init,
 * and Hidden = false. The "Version" key did not exist either.
 */
rapidjson::Document
makeLegacyDocument(const Model& proc, const std::vector<float>& normalized)
{
  rapidjson::Document doc = toJson(proc);
  doc.RemoveMember("Version");

  auto& inlets = doc["Inlets"];
  REQUIRE(inlets.IsArray());
  REQUIRE(inlets.Size() == normalized.size() + 1); // + the audio inlet

  const auto& meta = proc.parameters();
  QObject scratch;
  for(rapidjson::SizeType i = 1; i < inlets.Size(); i++)
  {
    auto& legacy
        = *new Process::FloatSlider{meta[i - 1].name, proc.inlets()[i]->id(), &scratch};
    legacy.setDomain(ossia::make_domain(0.f, 1.f));
    legacy.setValue(normalized[i - 1]);
    spliceInlet(doc, i, legacy);
  }

  return doc;
}

double valueOf(const Process::Port& p)
{
  return ossia::convert<double>(safe_cast<const Process::ControlInlet&>(p).value());
}

double minOf(const Process::Port& p)
{
  return safe_cast<const Process::ControlInlet&>(p).domain().get().convert_min<double>();
}

double maxOf(const Process::Port& p)
{
  return safe_cast<const Process::ControlInlet&>(p).domain().get().convert_max<double>();
}

void checkPortIdentity(const Process::Port& loaded, const Process::Port& original)
{
  CHECK(loaded.id() == original.id());
  CHECK(loaded.concreteKey() == original.concreteKey());
}

void checkControlState(
    const Process::ControlInlet& loaded, const Process::ControlInlet& original)
{
  // Keep the ossia value type as well as its number: an integer choice must not
  // turn into a float when a session is loaded.
  CHECK(loaded.value().get_type() == original.value().get_type());
  CHECK(loaded.value() == original.value());
  if(auto* box = qobject_cast<const Process::ComboBox*>(&original))
  {
    auto* loadedBox = qobject_cast<const Process::ComboBox*>(&loaded);
    REQUIRE(loadedBox);
    CHECK(loadedBox->alternatives == box->alternatives);
  }
  else
  {
    CHECK(minOf(loaded) == minOf(original));
    CHECK(maxOf(loaded) == maxOf(original));
  }
}
}

TEST_CASE("Controls are built from the derived metadata", "[airwindows][model]")
{
  QObject owner;
  auto& proc = *makeProcess("ADClip7", owner);

  REQUIRE(proc.parameters().size() == 4);
  REQUIRE(proc.inlets().size() == 5); // audio + 4 controls
  REQUIRE(proc.outlets().size() == 1);

  // Boost: float2string(A*18.0), "dB", defaulting to 0
  const auto& boost = *proc.inlets()[1];
  CHECK(boost.name() == "Boost (dB)");
  CHECK(boost.concreteKey() == Metadata<ConcreteKey_k, Process::FloatSlider>::get());
  CHECK(minOf(boost) == Approx(0.).margin(1e-3));
  CHECK(maxOf(boost) == Approx(18.).margin(1e-3));
  CHECK(valueOf(boost) == Approx(0.).margin(1e-3));

  // Mode: a three-entry popup, defaulting to "Normal"
  const auto& mode = *proc.inlets()[4];
  CHECK(mode.name() == "Mode");
  REQUIRE(mode.concreteKey() == Metadata<ConcreteKey_k, Process::ComboBox>::get());
  const auto& box = safe_cast<const Process::ComboBox&>(mode);
  REQUIRE(box.alternatives.size() == 3);
  CHECK(box.alternatives[0].first == "Normal");
  CHECK(box.alternatives[2].first == "Clips");
  CHECK(valueOf(mode) == Approx(0.));
}

TEST_CASE("Integer parameters get an int slider", "[airwindows][model]")
{
  QObject owner;
  auto& proc = *makeProcess("BitShiftGain", owner);

  REQUIRE(proc.inlets().size() == 2);
  const auto& bits = *proc.inlets()[1];
  CHECK(bits.name() == "BitShift (bits)");
  CHECK(bits.concreteKey() == Metadata<ConcreteKey_k, Process::IntSlider>::get());
  CHECK(minOf(bits) == Approx(-16.));
  CHECK(maxOf(bits) == Approx(16.));
  // The effect starts at A = 0.5, i.e. no shift
  CHECK(valueOf(bits) == Approx(0.));
}

TEST_CASE(
    "Every registered effect preserves its controls in saved sessions",
    "[airwindows][model][serialization]")
{
  Airwindows::initializeRegistry();
  REQUIRE(!AirwinRegistry::registry.empty());

  for(const auto& effect : AirwinRegistry::registry)
  {
    INFO("effect " << effect.name);
    QObject owner;
    auto& proc = *makeProcess(effect.name.c_str(), owner);
    REQUIRE(proc.reg == &effect);
    REQUIRE(proc.parameters().size() == static_cast<std::size_t>(effect.nParams));
    REQUIRE(proc.inlets().size() == proc.parameters().size() + 1);
    REQUIRE(proc.outlets().size() == 1);
    REQUIRE(qobject_cast<Process::AudioInlet*>(proc.inlets()[0]));
    REQUIRE(qobject_cast<Process::AudioOutlet*>(proc.outlets()[0]));

    std::vector<float> normalized;
    normalized.reserve(proc.parameters().size());
    for(std::size_t i = 0; i < proc.parameters().size(); i++)
    {
      INFO("parameter " << i);
      auto* control = qobject_cast<Process::ControlInlet*>(proc.inlets()[i + 1]);
      REQUIRE(control);
      const auto& meta = proc.parameters()[i];
      const auto initial = control->value();
      ossia::value chosen;
      float raw{};
      // Prefer interior values; endpoints also reach narrow discrete plateaus.
      // A constant parameter has no nondefault value to exercise.
      for(float candidate : {0.23f, 0.77f, 0.f, 1.f})
      {
        raw = candidate;
        const double display = std::clamp(meta.toDisplay(candidate), meta.min, meta.max);
        REQUIRE(ossia::safe_isfinite(display));
        if(meta.kind == Airwindows::ParameterKind::Enum
           || meta.kind == Airwindows::ParameterKind::Integer)
          chosen = static_cast<int>(std::llround(display));
        else
          chosen = static_cast<float>(display);
        if(chosen != initial)
          break;
      }
      REQUIRE((meta.min == meta.max || chosen != initial));
      control->setValue(chosen);
      REQUIRE(control->value() == chosen);
      normalized.push_back(raw);
    }

    for(bool binary : {false, true})
    {
      INFO("format " << (binary ? "binary" : "JSON"));
      auto* loaded
          = binary ? dataStreamRoundTrip(proc, owner) : fromJson(toJson(proc), owner);
      CHECK(loaded->pluginName() == proc.pluginName());
      CHECK(loaded->reg == proc.reg);
      REQUIRE(loaded->inlets().size() == proc.inlets().size());
      REQUIRE(loaded->outlets().size() == proc.outlets().size());
      for(std::size_t i = 0; i < proc.inlets().size(); i++)
      {
        INFO("inlet " << i);
        checkPortIdentity(*loaded->inlets()[i], *proc.inlets()[i]);
        CHECK(loaded->inlets()[i]->name() == proc.inlets()[i]->name());
        if(i > 0)
        {
          auto* control = qobject_cast<Process::ControlInlet*>(loaded->inlets()[i]);
          REQUIRE(control);
          checkControlState(
              *control, safe_cast<const Process::ControlInlet&>(*proc.inlets()[i]));
        }
      }
      for(std::size_t i = 0; i < proc.outlets().size(); i++)
      {
        INFO("outlet " << i);
        checkPortIdentity(*loaded->outlets()[i], *proc.outlets()[i]);
        CHECK(loaded->outlets()[i]->name() == proc.outlets()[i]->name());
      }
    }

    // Format-1 conversion of every effect. Whether the metadata maps correctly is
    // MetadataTest's job; here its display value must survive the load boundary.
    INFO("format legacy JSON");
    auto& migrated = *fromJson(makeLegacyDocument(proc, normalized), owner);
    CHECK(migrated.pluginName() == proc.pluginName());
    CHECK(migrated.reg == proc.reg);
    REQUIRE(migrated.inlets().size() == proc.inlets().size());
    for(std::size_t i = 0; i < proc.inlets().size(); i++)
    {
      INFO("inlet " << i);
      checkPortIdentity(*migrated.inlets()[i], *proc.inlets()[i]);
      if(i > 0)
      {
        auto* control = qobject_cast<Process::ControlInlet*>(migrated.inlets()[i]);
        REQUIRE(control);
        checkControlState(
            *control, safe_cast<const Process::ControlInlet&>(*proc.inlets()[i]));
      }
    }
  }
}

TEST_CASE(
    "Format 1 controls are converted on load", "[airwindows][model][serialization]")
{
  QObject owner;
  auto& proc = *makeProcess("ADClip7", owner);

  // What an old document held: four [0;1] sliders with the raw parameters.
  const auto legacy = makeLegacyDocument(proc, {0.5f, 0.25f, 0.75f, 0.9f});
  auto& loaded = *fromJson(legacy, owner);

  REQUIRE(loaded.inlets().size() == 5);

  // Boost: 0.5 normalized was 9 dB all along, and now says so.
  const auto& boost = *loaded.inlets()[1];
  CHECK(boost.concreteKey() == Metadata<ConcreteKey_k, Process::FloatSlider>::get());
  CHECK(boost.name() == "Boost (dB)");
  CHECK(minOf(boost) == Approx(0.).margin(1e-3));
  CHECK(maxOf(boost) == Approx(18.).margin(1e-3));
  CHECK(valueOf(boost) == Approx(9.).margin(1e-2));

  // Soften and Enhance already displayed as [0;1]: nothing to convert, and nothing
  // was touched - same widget, same domain, same value.
  CHECK(valueOf(*loaded.inlets()[2]) == Approx(0.25).margin(1e-3));
  CHECK(valueOf(*loaded.inlets()[3]) == Approx(0.75).margin(1e-3));

  // Mode: 0.9 normalized sat in the third plateau, which is now choice 2.
  const auto& mode = *loaded.inlets()[4];
  REQUIRE(mode.concreteKey() == Metadata<ConcreteKey_k, Process::ComboBox>::get());
  CHECK(safe_cast<const Process::ComboBox&>(mode).alternatives.size() == 3);
  CHECK(valueOf(mode) == Approx(2.));

  // Port identity survives, so cables that referenced these ports still resolve:
  // the document re-attaches them by path after loading, and a path ends in the
  // port's id.
  for(std::size_t i = 0; i < loaded.inlets().size(); i++)
    CHECK(loaded.inlets()[i]->id() == proc.inlets()[i]->id());
}

TEST_CASE("Converting a format 1 integer control", "[airwindows][model][serialization]")
{
  QObject owner;
  auto& proc = *makeProcess("BitShiftGain", owner);

  const auto legacy = makeLegacyDocument(proc, {0.75f});
  auto& loaded = *fromJson(legacy, owner);

  REQUIRE(loaded.inlets().size() == 2);
  const auto& bits = *loaded.inlets()[1];
  CHECK(bits.concreteKey() == Metadata<ConcreteKey_k, Process::IntSlider>::get());
  CHECK(minOf(bits) == Approx(-16.));
  CHECK(maxOf(bits) == Approx(16.));
  // (VstInt32)((0.75 * 32) - 16) == 8
  CHECK(valueOf(bits) == Approx(8.));
}

TEST_CASE("Rebuilding a control keeps what the user put on it", "[airwindows][model]")
{
  QObject owner;
  auto& proc = *makeProcess("ADClip7", owner);

  auto legacy = makeLegacyDocument(proc, {0.5f, 0.25f, 0.75f, 0.9f});

  // Decorate the Boost port the way a user would: exposed on the local OSC tree,
  // described, and bound to a device address.
  {
    const auto& meta = proc.parameters();
    QObject scratch;
    auto& port
        = *new Process::FloatSlider{meta[0].name, proc.inlets()[1]->id(), &scratch};
    port.setDomain(ossia::make_domain(0.f, 1.f));
    port.setValue(0.5f);
    port.setExposed("boost");
    port.setDescription("drives the clipper");
    port.setAddress(State::AddressAccessor{State::Address{"dev", {"a", "b"}}});
    spliceInlet(legacy, 1, port);
  }

  auto& loaded = *fromJson(legacy, owner);
  const auto& boost = *loaded.inlets()[1];

  // It was rebuilt - the domain proves that...
  CHECK(maxOf(boost) == Approx(18.).margin(1e-3));
  CHECK(valueOf(boost) == Approx(9.).margin(1e-2));

  // ...and none of the user's own state was dropped in the process. Losing
  // `exposed` in particular would silently unbind the control from the OSC tree.
  CHECK(boost.exposed() == "boost");
  CHECK(boost.description() == "drives the clipper");
  CHECK(boost.address().address.device == "dev");
  CHECK(boost.address().address.path == QStringList{"a", "b"});
}

TEST_CASE(
    "A format 2 value survives the metadata moving under it",
    "[airwindows][model][serialization]")
{
  QObject owner;
  auto& proc = *makeProcess("ADClip7", owner);

  // A document written by a build whose airwin2rack revision gave Boost a wider
  // range. The stored 12.5 is already in dB.
  auto doc = toJson(proc);
  {
    QObject scratch;
    auto& port = *new Process::FloatSlider{
        0.f, 24.f, 12.5f, "Boost (dB)", proc.inlets()[1]->id(), &scratch};
    spliceInlet(doc, 1, port);
  }

  auto& loaded = *fromJson(doc, owner);
  const auto& boost = *loaded.inlets()[1];

  // Rebuilt against today's range...
  CHECK(minOf(boost) == Approx(0.).margin(1e-3));
  CHECK(maxOf(boost) == Approx(18.).margin(1e-3));

  // ...but the value is still 12.5 dB. Reading it as normalized would clamp it to
  // 1.0 and hand back 18 dB - the parameter would jump to its maximum on load.
  CHECK(valueOf(boost) == Approx(12.5).margin(1e-2));
}

TEST_CASE(
    "A parameter renamed upstream takes its new name and keeps its value",
    "[airwindows][model][serialization]")
{
  QObject owner;
  auto& proc = *makeProcess("ADClip7", owner);

  // A format 2 document whose Boost port was written under another name, with the
  // same widget and range as today.
  auto doc = toJson(proc);
  {
    QObject scratch;
    auto& port = *new Process::FloatSlider{
        0.f, 18.f, 12.5f, "Gain (dB)", proc.inlets()[1]->id(), &scratch};
    port.setExposed("boost");
    spliceInlet(doc, 1, port);
  }

  auto& loaded = *fromJson(doc, owner);
  const auto& boost = *loaded.inlets()[1];
  CHECK(boost.id() == proc.inlets()[1]->id());
  CHECK(boost.name() == proc.inlets()[1]->name());
  CHECK(boost.exposed() == "boost");
  CHECK(valueOf(boost) == Approx(12.5).margin(1e-3));
  CHECK(maxOf(boost) == Approx(18.).margin(1e-3));
}

TEST_CASE("Conversion is idempotent", "[airwindows][model][serialization]")
{
  QObject owner;
  auto& proc = *makeProcess("ADClip7", owner);

  auto& once = *fromJson(makeLegacyDocument(proc, {0.5f, 0.25f, 0.75f, 0.9f}), owner);
  auto& twice = *fromJson(toJson(once), owner);

  REQUIRE(twice.inlets().size() == once.inlets().size());
  for(std::size_t i = 1; i < once.inlets().size(); i++)
  {
    INFO("inlet " << i);
    CHECK(twice.inlets()[i]->concreteKey() == once.inlets()[i]->concreteKey());
    CHECK(twice.inlets()[i]->name() == once.inlets()[i]->name());
    CHECK(valueOf(*twice.inlets()[i]) == Approx(valueOf(*once.inlets()[i])));
    CHECK(minOf(*twice.inlets()[i]) == Approx(minOf(*once.inlets()[i])));
    CHECK(maxOf(*twice.inlets()[i]) == Approx(maxOf(*once.inlets()[i])));
  }
}

TEST_CASE("A plug-in that vanished keeps its controls", "[airwindows][model]")
{
  QObject owner;
  auto& proc = *makeProcess("ADClip7", owner);
  safe_cast<Process::ControlInlet*>(proc.inlets()[1])->setValue(12.5f);

  // An effect the registry no longer knows: there is no metadata to reconcile
  // against, so the ports have to be left exactly as they were rather than
  // deleted - re-saving would otherwise throw the user's settings away.
  auto doc = toJson(proc);
  doc["PluginName"].SetString("NoSuchEffect", doc.GetAllocator());

  auto& loaded = *fromJson(doc, owner);
  CHECK(loaded.reg == nullptr);
  CHECK(loaded.parameters().empty());
  REQUIRE(loaded.inlets().size() == 5);
  CHECK(valueOf(*loaded.inlets()[1]) == Approx(12.5));

  // ...and they are still there after another round-trip.
  auto& again = *fromJson(toJson(loaded), owner);
  REQUIRE(again.inlets().size() == 5);
  CHECK(valueOf(*again.inlets()[1]) == Approx(12.5));
}

TEST_CASE("An unknown plug-in yields no controls", "[airwindows][model]")
{
  QObject owner;
  auto& proc = *makeProcess("NoSuchEffect", owner);

  CHECK(proc.parameters().empty());
  CHECK(controlCount(proc) == 0);
  CHECK(proc.reg == nullptr);
  CHECK(proc.inlets().size() == 1);
}
