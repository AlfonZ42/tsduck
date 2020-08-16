//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2018, Tristan Claverie
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------

#include "tsAIT.h"
#include "tsBinaryTable.h"
#include "tsNames.h"
#include "tsTablesDisplay.h"
#include "tsPSIRepository.h"
#include "tsPSIBuffer.h"
#include "tsDuckContext.h"
#include "tsxmlElement.h"
TSDUCK_SOURCE;

#define MY_XML_NAME u"AIT"
#define MY_CLASS ts::AIT
#define MY_TID ts::TID_AIT
#define MY_STD ts::Standards::DVB

TS_REGISTER_TABLE(MY_CLASS, {MY_TID}, MY_STD, MY_XML_NAME, MY_CLASS::DisplaySection);


//----------------------------------------------------------------------------
// Constructors:
//----------------------------------------------------------------------------

ts::AIT::AIT(uint8_t version_, bool is_current_, uint16_t application_type_, bool test_application_) :
    AbstractLongTable(MY_TID, MY_XML_NAME, MY_STD, version_, is_current_),
    application_type(application_type_),
    test_application_flag(test_application_),
    descs(this),
    applications(this)
{
}

ts::AIT::AIT(DuckContext& duck, const BinaryTable& table) :
    AIT()
{
    deserialize(duck, table);
}

ts::AIT::AIT(const AIT& other) :
    AbstractLongTable(other),
    application_type(other.application_type),
    test_application_flag(other.test_application_flag),
    descs(this, other.descs),
    applications(this, other.applications)
{
}

ts::AIT::Application::Application(const AbstractTable* table) :
    EntryWithDescriptors(table),
    control_code(0)
{
}



//----------------------------------------------------------------------------
// Get the table id extension.
//----------------------------------------------------------------------------

uint16_t ts::AIT::tableIdExtension() const
{
    return (test_application_flag ? 0x8000 : 0x0000) | (application_type & 0x7FFF);
}


//----------------------------------------------------------------------------
// Clear the content of the table.
//----------------------------------------------------------------------------

void ts::AIT::clearContent()
{
    application_type = 0;
    test_application_flag = false;
    descs.clear();
    applications.clear();
}


//----------------------------------------------------------------------------
// Deserialization
//----------------------------------------------------------------------------

void ts::AIT::deserializePayload(PSIBuffer& buf, const Section& section)
{
    // Get common properties (should be identical in all sections)
    const uint16_t tid_ext = section.tableIdExtension();
    test_application_flag = (tid_ext & 0x8000) != 0;
    application_type = tid_ext & 0x7fff;

    // Get common descriptor list
    buf.getDescriptorListWithLength(descs);

    // Application loop length.
    buf.skipBits(4);
    const size_t loop_length = buf.getBits<size_t>(12);
    const size_t end_loop = buf.currentReadByteOffset() + loop_length;

    // Get application descriptions.
    while (!buf.error() && !buf.endOfRead()) {
        const uint32_t org_id = buf.getUInt32();
        const uint16_t app_id = buf.getUInt16();
        Application& app(applications[ApplicationIdentifier(org_id, app_id)]);
        app.control_code = buf.getUInt8();
        buf.getDescriptorListWithLength(app.descs);
    }

    // Make sure we exactly reached the end of transport stream loop.
    if (!buf.error() && buf.currentReadByteOffset() != end_loop) {
        buf.setUserError();
    }
}


//----------------------------------------------------------------------------
// Serialization
//----------------------------------------------------------------------------

void ts::AIT::serializePayload(BinaryTable& table, PSIBuffer& payload) const
{
    // Minimum size of a section: empty common descriptor list and application_loop_length.
    constexpr size_t payload_min_size = 4;

    // Add common descriptor list.
    // If the descriptor list is too long to fit into one section, create new sections when necessary.
    for (size_t start = 0;;) {
        // Reserve and restore 2 bytes for application_loop_length.
        payload.pushSize(payload.size() - 2);
        start = payload.putPartialDescriptorListWithLength(descs, start);
        payload.popSize();

        if (payload.error() || start >= descs.size()) {
            // Common descriptor list completed.
            break;
        }
        else {
            // There are remaining top-level descriptors, flush current section.
            // Add a zero application_loop_length.
            payload.putUInt16(0xF000);
            addOneSection(table, payload);
        }
    }

    // Reserve application_loop_length.
    payload.pushReadWriteState();
    payload.putUInt16(0xF000);

    // Add all transports
    for (auto it = applications.begin(); it != applications.end(); ++it) {

        // If we cannot at least add the fixed part of an application description, open a new section
        if (payload.remainingWriteBytes() < 9) {
            addSection(table, payload, false);
        }

        // Binary size of the application entry.
        const size_t entry_size = 9 + it->second.descs.binarySize();

        // If we are not at the beginning of the application loop, make sure that the entire
        // application description fits in the section. If it does not fit, start a new section.
        if (entry_size > payload.remainingWriteBytes() && payload.currentWriteByteOffset() > payload_min_size) {
            // Create a new section
            addSection(table, payload, false);
        }

        // Serialize the characteristics of the application.
        // If the descriptor list is too large for an entire section, it is truncated.
        payload.putUInt32(it->first.organization_id);
        payload.putUInt16(it->first.application_id);
        payload.putUInt8(it->second.control_code);
        payload.putPartialDescriptorListWithLength(it->second.descs);
    }

    // Add partial section.
    addSection(table, payload, true);
}


//----------------------------------------------------------------------------
// Add a new section to a table being serialized, while in application loop.
//----------------------------------------------------------------------------

void ts::AIT::addSection(BinaryTable& table, PSIBuffer& payload, bool last_section) const
{
    // The read/write state was pushed just before application_loop_length.

    // Update application_loop_length.
    const size_t end = payload.currentWriteByteOffset();
    payload.swapReadWriteState();
    assert(payload.currentWriteByteOffset() + 2 <= end);
    const size_t loop_length = end - payload.currentWriteByteOffset() - 2;
    payload.putBits(0xFF, 4);
    payload.putBits(loop_length, 12);
    payload.popReadWriteState();

    // Add the section and reset buffer.
    addOneSection(table, payload);

    // Prepare for the next section if necessary.
    if (!last_section) {
        // Empty (zero-length) top-level descriptor list.
        payload.putUInt16(0xF000);

        // Reserve application_loop_length.
        payload.pushReadWriteState();
        payload.putUInt16(0xF000);
    }
}


//----------------------------------------------------------------------------
// A static method to display a AIT section.
//----------------------------------------------------------------------------

void ts::AIT::DisplaySection(TablesDisplay& display, const ts::Section& section, int indent)
{
    DuckContext& duck(display.duck());
    std::ostream& strm(duck.out());
    const std::string margin(indent, ' ');
    PSIBuffer buf(duck, section.payload(), section.payloadSize());

    // Common information.
    const uint16_t tidext = section.tableIdExtension();
    strm << margin << UString::Format(u"Application type: %d (0x%<04X), Test application: %d", {tidext & 0x7FFF, tidext >> 15}) << std::endl;
    display.displayDescriptorListWithLength(section, buf, indent, u"Common descriptor loop:");

    // Application loop length.
    buf.skipBits(4);
    const size_t loop_length = buf.getBits<size_t>(12);
    const size_t end_loop = buf.currentReadByteOffset() + loop_length;

    // Loop across all applications.
    while (!buf.error() && buf.currentReadByteOffset() + 9 <= end_loop && buf.remainingReadBytes() >= 9) {
        const uint32_t org_id = buf.getUInt32();
        const uint16_t app_id = buf.getUInt16();
        const uint8_t code = buf.getUInt8();
        strm << margin
             << UString::Format(u"Application: Identifier: (Organization id: %d (0x%<X), Application id: %d (0x%<X)), Control code: %d", {org_id, app_id, code})
             << std::endl;
        display.displayDescriptorListWithLength(section, buf, indent);
    }

    display.displayExtraData(buf, indent);
}


//----------------------------------------------------------------------------
// XML serialization
//----------------------------------------------------------------------------

void ts::AIT::buildXML(DuckContext& duck, xml::Element* root) const
{
    root->setIntAttribute(u"version", version);
    root->setBoolAttribute(u"current", is_current);
    root->setBoolAttribute(u"test_application_flag", test_application_flag);
    root->setIntAttribute(u"application_type", application_type, true);
    descs.toXML(duck, root);

    for (ApplicationMap::const_iterator it = applications.begin(); it != applications.end(); ++it) {
        xml::Element* e = root->addElement(u"application");
        e->setIntAttribute(u"control_code", it->second.control_code, true);
        xml::Element* id = e->addElement(u"application_identifier");
        id->setIntAttribute(u"organization_id", it->first.organization_id, true);
        id->setIntAttribute(u"application_id", it->first.application_id, true);

        it->second.descs.toXML(duck, e);
    }
}


//----------------------------------------------------------------------------
// XML deserialization
//----------------------------------------------------------------------------

bool ts::AIT::analyzeXML(DuckContext& duck, const xml::Element* element)
{
    xml::ElementVector children;
    bool ok =
        element->getIntAttribute<uint8_t>(version, u"version", false, 0, 0, 31) &&
        element->getBoolAttribute(is_current, u"current", false, true) &&
        element->getBoolAttribute(test_application_flag, u"test_application_flag", false, true) &&
        element->getIntAttribute<uint16_t>(application_type, u"application_type", true, 0, 0x0000, 0x7FFF) &&
        descs.fromXML(duck, children, element, u"application");

    // Iterate through applications
    for (size_t index = 0; ok && index < children.size(); ++index) {
        Application application(this);
        ApplicationIdentifier identifier;
        const xml::Element* id = children[index]->findFirstChild(u"application_identifier", true);
        xml::ElementVector others;
        UStringList allowed({ u"application_identifier" });

        ok = children[index]->getIntAttribute<uint8_t>(application.control_code, u"control_code", true, 0, 0x00, 0xFF) &&
             application.descs.fromXML(duck, others, children[index], allowed) &&
             id != nullptr &&
             id->getIntAttribute<uint32_t>(identifier.organization_id, u"organization_id", true, 0, 0, 0xFFFFFFFF) &&
             id->getIntAttribute<uint16_t>(identifier.application_id, u"application_id", true, 0, 0, 0xFFFF);

        if (ok) {
            applications[identifier] = application;
        }
    }
    return ok;
}
