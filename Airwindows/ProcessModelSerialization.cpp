// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "ProcessModel.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>

#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>

#include <QIODevice>
#include <QString>

template <>
void DataStreamReader::read(const Airwindows::ProcessModel& proc)
{
  m_stream << proc.m_pluginName;
  readPorts(*this, proc.m_inlets, proc.m_outlets);

  insertDelimiter();
}

template <>
void DataStreamWriter::write(Airwindows::ProcessModel& proc)
{
  m_stream >> proc.m_pluginName;
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);

  checkDelimiter();

  // The DataStream layout carries no version and cannot gain one without breaking
  // every existing .scorebin - score has no migration hook for that format, see
  // DocumentSerialization.cpp, where only the JSON branch is version-checked. So
  // fall back on the structural test in migrateControls(), which recognizes a
  // format-1 control by its bare [0;1] domain.
  proc.m_loadedVersion = 1;
}

template <>
void JSONReader::read(const Airwindows::ProcessModel& proc)
{
  obj["PluginName"] = proc.m_pluginName;
  obj["Version"] = Airwindows::ProcessModel::formatVersion;
  readPorts(*this, proc.m_inlets, proc.m_outlets);
}

template <>
void JSONWriter::write(Airwindows::ProcessModel& proc)
{
  proc.m_pluginName = obj["PluginName"].toString();

  // Absent means format 1: it predates the field. This is what keeps a format-2
  // value in display units from being read back as if it were normalized when the
  // metadata has moved under it.
  proc.m_loadedVersion = 1;
  if(auto it = obj.tryGet("Version"))
    proc.m_loadedVersion = it->toInt();

  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);
}
