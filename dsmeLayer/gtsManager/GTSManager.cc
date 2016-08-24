/*
 * openDSME
 *
 * Implementation of the Deterministic & Synchronous Multi-channel Extension (DSME)
 * described in the IEEE 802.15.4-2015 standard
 *
 * Authors: Florian Meier <florian.meier@tuhh.de>
 *          Maximilian Koestler <maximilian.koestler@tuhh.de>
 *          Sandrina Backhauss <sandrina.backhauss@tuhh.de>
 *
 * Based on
 *          DSME Implementation for the INET Framework
 *          Tobias Luebkert <tobias.luebkert@tuhh.de>
 *
 * Copyright (c) 2015, Institute of Telematics, Hamburg University of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "GTSManager.h"

#include "../../../dsme_platform.h"
#include "../DSMELayer.h"
#include "../messages/GTSReplyNotifyCmd.h"
#include "../messages/GTSRequestCmd.h"
#include "../messages/GTSRequestCmd.h"
#include "../messages/MACCommand.h"

namespace dsme {

GTSManager::GTSManager(DSMELayer& dsme) :
        GTSManagerFSM_t(&GTSManager::stateIdle, &GTSManager::stateBusy),
        dsme(dsme),
        actUpdater(dsme) {
}

void GTSManager::initialize() {
    dsme.getMAC_PIB().macDSMESAB.initialize(dsme.getMAC_PIB().helper.getNumberSuperframesPerMultiSuperframe(),
            dsme.getMAC_PIB().helper.getNumGTSlots(),
            dsme.getMAC_PIB().helper.getNumChannels());
    dsme.getMAC_PIB().macDSMEACT.initialize(dsme.getMAC_PIB().helper.getNumberSuperframesPerMultiSuperframe(),
            dsme.getMAC_PIB().helper.getNumGTSlots(),
            dsme.getMAC_PIB().helper.getNumChannels());
    return;
}

/*****************************
 * States
 *****************************/
fsmReturnStatus GTSManager::stateBusy(GTSEvent& event) {
    int8_t fsmId = event.getFsmId();
    DSME_ASSERT(fsmId == GTS_STATE_MULTIPLICITY);

    LOG_DEBUG("GTS Event handled: '" << signalToString(event.signal) << "' (" << stateToString(&GTSManager::stateBusy) << ")" << "[" << (uint16_t)fsmId << "]");

    switch (event.signal) {
        case GTSEvent::ENTRY_SIGNAL:
            return FSM_IGNORED;
        case GTSEvent::EXIT_SIGNAL:
            DSME_ASSERT(false);
            return FSM_IGNORED;
        case GTSEvent::MLME_REQUEST_ISSUED:
            actionReportBusyNotify(event);
            return FSM_IGNORED;
        case GTSEvent::MLME_RESPONSE_ISSUED:
            actionSendImmediateNegativeResponse(event);
            actionReportBusyCommStatus(event);
            return FSM_HANDLED;
        case GTSEvent::SEND_COMPLETE:
            LOG_DEBUG("Outdated message");
            return FSM_IGNORED;
        default:
            return FSM_IGNORED;
    }
}

fsmReturnStatus GTSManager::stateIdle(GTSEvent& event) {
    int8_t fsmId = event.getFsmId();
    LOG_DEBUG("GTS Event handled: '" << signalToString(event.signal) << "' (" << stateToString(&GTSManager::stateIdle) << ")" << "[" << (uint16_t)fsmId << "]");

    switch(event.signal) {
    case GTSEvent::ENTRY_SIGNAL:
    case GTSEvent::EXIT_SIGNAL:
        return FSM_IGNORED;
    case GTSEvent::MLME_REQUEST_ISSUED: {
        preparePendingConfirm(event);

        DSMEMessage* msg = dsme.getPlatform().getEmptyMessage();

        event.requestCmd.prependTo(msg);

        if(!sendGTSCommand(fsmId, msg, event.management, CommandFrameIdentifier::DSME_GTS_REQUEST, event.deviceAddr)) {
            dsme.getPlatform().releaseMessage(msg);

            LOG_DEBUG("TRANSACTION_OVERFLOW");
            data[fsmId].pendingConfirm.status = GTSStatus::TRANSACTION_OVERFLOW;

            this->dsme.getMLME_SAP().getDSME_GTS().notify_confirm(data[fsmId].pendingConfirm);
            return FSM_HANDLED;
        }
        else {
            return transition(fsmId, &GTSManager::stateSending);
        }
    }

    case GTSEvent::MLME_RESPONSE_ISSUED: {
        preparePendingConfirm(event);

        DSMEMessage* msg = dsme.getPlatform().getEmptyMessage();
        event.replyNotifyCmd.prependTo(msg);

        uint16_t destinationShortAddress;

        if (event.management.status == GTSStatus::SUCCESS) {
            LOG_INFO("Sending a positive response to a GTS-REQUEST to " << event.replyNotifyCmd.getDestinationAddress());

            destinationShortAddress = IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS;
        } else {
            LOG_INFO("Sending a negative response to a GTS-REQUEST to " << event.replyNotifyCmd.getDestinationAddress());

            destinationShortAddress = event.replyNotifyCmd.getDestinationAddress();
        }

        if(!sendGTSCommand(fsmId, msg, event.management, CommandFrameIdentifier::DSME_GTS_REPLY, destinationShortAddress)) {
            LOG_DEBUG("Could not send REPLY");
            dsme.getPlatform().releaseMessage(msg);

            mlme_sap::COMM_STATUS_indication_parameters params;
            // TODO also fill other fields
            params.status = CommStatus::Comm_Status::TRANSACTION_OVERFLOW;
            this->dsme.getMLME_SAP().getCOMM_STATUS().notify_indication(params);
            return FSM_HANDLED;
        } else {
            if (event.management.status == GTSStatus::SUCCESS) {
                actUpdater.approvalQueued(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
            }
            return transition(fsmId, &GTSManager::stateSending);
        }
    }

        case GTSEvent::RESPONSE_CMD_FOR_ME:
        case GTSEvent::NOTIFY_CMD_FOR_ME:
        case GTSEvent::SEND_COMPLETE:
            DSME_ASSERT(false);
            return FSM_IGNORED;

        case GTSEvent::CFP_STARTED: {
            // check if a slot should be deallocated, only if no reply or notify is pending
            for (DSMEAllocationCounterTable::iterator it = dsme.getMAC_PIB().macDSMEACT.begin(); it != dsme.getMAC_PIB().macDSMEACT.end();
                    it++) {
                // Since no reply is pending, this slot should have been removed already and is no longer in the ACT
                // This should be even the case for timeouts (NO_DATA indication for upper layer)
                DSME_ASSERT(it->getState() != DEALLOCATED);
                DSME_ASSERT(it->getState() != REMOVED);

                LOG_DEBUG("check slot "
                        << (uint16_t)it->getGTSlotID()
                        << " " << it->getSuperframeID()
                        << " " << (uint16_t)it->getChannel()
                        << " [" << this->dsme.getMAC_PIB().macShortAddress
                        << (const char*)((it->getDirection()==Direction::TX)?">":"<")
                        << it->getAddress()
                        << ", " << it->getIdleCounter() << "]");

                // TODO Since INVALID is not included in the standard, use the EXPIRATION type for INVALID, too.
                //      The effect should be the same.
                if (it->getState() == INVALID || it->getState() == UNCONFIRMED
                        || it->getIdleCounter() > dsme.getMAC_PIB().macDSMEGTSExpirationTime) {

                    if (it->getState() == INVALID) {
                        LOG_DEBUG("DEALLOCATE: Due to state INVALID");
                    } else if (it->getState() == UNCONFIRMED) {
                        bool pendingAllocation = false;
                        for (uint8_t i = 0; i < GTS_STATE_MULTIPLICITY; ++i) {
                            if (getState(i) != &GTSManager::stateIdle) {
                                pendingAllocation = true;
                            }
                        }
                        if (pendingAllocation) {
                            continue;
                        }
                        LOG_DEBUG("DEALLOCATE: Due to state UNCONFIRMED");
                    } else if (it->getIdleCounter() > dsme.getMAC_PIB().macDSMEGTSExpirationTime) {
                        it->resetIdleCounter();
                        LOG_DEBUG("DEALLOCATE: Due to expiration");
                    } else {
                        DSME_ASSERT(false);
                    }

                    mlme_sap::DSME_GTS_indication_parameters params;
                    params.deviceAddress = it->getAddress();

                    params.managmentType = EXPIRATION;
                    params.direction = it->getDirection();
                    params.prioritizedChannelAccess = Priority::LOW;
                    params.numSlot = 1;

                    uint8_t subBlockLengthBytes = dsme.getMAC_PIB().helper.getSubBlockLengthBytes();
                    params.dsmeSABSpecification.setSubBlockLengthBytes(subBlockLengthBytes);
                    params.dsmeSABSpecification.setSubBlockIndex(it->getSuperframeID());
                    params.dsmeSABSpecification.getSubBlock().fill(false);
                    params.dsmeSABSpecification.getSubBlock().set(
                            it->getGTSlotID() * dsme.getMAC_PIB().helper.getNumChannels() + it->getChannel(), true);

                    this->dsme.getMLME_SAP().getDSME_GTS().notify_indication(params);
                    break;
                }
            }

            return FSM_HANDLED;
        }

    default:
        DSME_ASSERT(false);
        return FSM_IGNORED;
    }
}

fsmReturnStatus GTSManager::stateSending(GTSEvent& event) {
    int8_t fsmId = event.getFsmId();
    LOG_DEBUG("GTS Event handled: '" << signalToString(event.signal) << "' (" << stateToString(&GTSManager::stateSending) << ")" << "[" << (uint16_t)fsmId << "]");

    switch (event.signal) {
        case GTSEvent::ENTRY_SIGNAL:
        case GTSEvent::EXIT_SIGNAL:
            return FSM_IGNORED;

        case GTSEvent::MLME_REQUEST_ISSUED:
        case GTSEvent::MLME_RESPONSE_ISSUED:
        case GTSEvent::RESPONSE_CMD_FOR_ME:
        case GTSEvent::NOTIFY_CMD_FOR_ME:
        case GTSEvent::CFP_STARTED:
            DSME_ASSERT(false);
            return FSM_IGNORED;

        case GTSEvent::SEND_COMPLETE: {
            DSME_ASSERT(event.cmdId == DSME_GTS_REQUEST || event.cmdId == DSME_GTS_REPLY || event.cmdId == DSME_GTS_NOTIFY);
            DSME_ASSERT(event.cmdId == data[fsmId].cmdToSend);

            if (event.cmdId == DSME_GTS_NOTIFY) {
                actUpdater.notifyDelivered(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
                return transition(fsmId, &GTSManager::stateIdle);
            } else if (event.cmdId == DSME_GTS_REQUEST) {
                if (event.dataStatus != DataStatus::Data_Status::SUCCESS) {
                    LOG_DEBUG("GTSManager sending request failed " << (uint16_t)event.dataStatus);

                    switch (event.dataStatus) {
                        case DataStatus::NO_ACK:
                            actUpdater.requestNoAck(event.requestCmd.getSABSpec(), event.management, event.deviceAddr);
                            data[fsmId].pendingConfirm.status = GTSStatus::NO_ACK;
                            break;
                        case DataStatus::CHANNEL_ACCESS_FAILURE:
                            actUpdater.requestAccessFailure(event.requestCmd.getSABSpec(), event.management, event.deviceAddr);
                            data[fsmId].pendingConfirm.status = GTSStatus::CHANNEL_ACCESS_FAILURE;
                            break;
                        default:
                            DSME_ASSERT(false);
                    }

                    this->dsme.getMLME_SAP().getDSME_GTS().notify_confirm(data[fsmId].pendingConfirm);
                    return transition(fsmId, &GTSManager::stateIdle);
                } else {
                    // REQUEST_SUCCESS
                    data[fsmId].responsePartnerAddress = event.deviceAddr;
                    return transition(fsmId, &GTSManager::stateWaitForResponse);
                }
            } else if (event.cmdId == DSME_GTS_REPLY) {
                if (event.dataStatus != DataStatus::Data_Status::SUCCESS) {
                    mlme_sap::COMM_STATUS_indication_parameters params;
                    // TODO also fill other fields

                    switch (event.dataStatus) {
                        case DataStatus::NO_ACK:
                            // An ACK is only expected for disapprovals
                            DSME_ASSERT(event.management.status == GTSStatus::GTS_Status::DENIED);
                            actUpdater.disapprovalNoAck(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);

                            params.status = CommStatus::Comm_Status::NO_ACK;
                            break;
                        case DataStatus::CHANNEL_ACCESS_FAILURE:
                            if (event.management.status == GTSStatus::SUCCESS) {
                                actUpdater.approvalAccessFailure(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
                            } else if (event.management.status == GTSStatus::DENIED) {
                                actUpdater.disapprovalAccessFailure(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
                            } else {
                                DSME_ASSERT(false);
                            }

                            params.status = CommStatus::Comm_Status::CHANNEL_ACCESS_FAILURE;
                            break;
                        default:
                            DSME_ASSERT(false);
                    }

                    this->dsme.getMLME_SAP().getCOMM_STATUS().notify_indication(params);
                    return transition(fsmId, &GTSManager::stateIdle);
                } else {
                    if (event.management.status == GTSStatus::SUCCESS) {
                        actUpdater.approvalDelivered(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
                        data[fsmId].notifyPartnerAddress = event.deviceAddr;
                        return transition(fsmId, &GTSManager::stateWaitForNotify);
                    } else if (event.management.status == GTSStatus::DENIED) {
                        actUpdater.disapprovalDelivered(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);

                        // for disapprovals, no notify is expected
                        return transition(fsmId, &GTSManager::stateIdle);
                    } else {
                        DSME_ASSERT(false);
                    }
                }
            }

            DSME_ASSERT(false);
            return FSM_IGNORED;
        }
        default:
            DSME_ASSERT(false);
            return FSM_IGNORED;
    }
}

fsmReturnStatus GTSManager::stateWaitForResponse(GTSEvent& event) {
    int8_t fsmId = event.getFsmId();
    LOG_DEBUG("GTS Event handled: '" << signalToString(event.signal) << "' (" << stateToString(&GTSManager::stateWaitForResponse) << ")" << "[" << (uint16_t)fsmId << "]");

    switch (event.signal) {
        case GTSEvent::ENTRY_SIGNAL:
            data[fsmId].superframesInCurrentState = 0;
            return FSM_HANDLED;
        case GTSEvent::EXIT_SIGNAL:
            return FSM_IGNORED;

        case GTSEvent::MLME_REQUEST_ISSUED:
        case GTSEvent::MLME_RESPONSE_ISSUED:
        case GTSEvent::NOTIFY_CMD_FOR_ME:
        case GTSEvent::SEND_COMPLETE:
            DSME_ASSERT(false);
            return FSM_IGNORED;

        case GTSEvent::RESPONSE_CMD_FOR_ME: {
            mlme_sap::DSME_GTS_confirm_parameters params;
            params.deviceAddress = event.deviceAddr;
            params.managmentType = event.management.type;
            params.direction = event.management.direction;
            params.prioritizedChannelAccess = event.management.prioritizedChannelAccess;
            params.dsmeSABSpecification = event.replyNotifyCmd.getSABSpec();

            // TODO // if the ACK gets lost, the reply might be sent anyway, so we might be in SENDING_REQUEST
            // TODO DSME_ASSERT((state == State::SENDING && cmdToSend == DSME_GTS_REQUEST) || state == State::WAIT_FOR_REPLY);

            if (data[fsmId].pendingConfirm.deviceAddress != params.deviceAddress) {
                LOG_INFO("Wrong response handled! Got address " << params.deviceAddress << " instead of " << data[fsmId].pendingConfirm.deviceAddress);
                //DSME_ASSERT(false);
                return FSM_HANDLED;
            }
            if (data[fsmId].pendingConfirm.managmentType != params.managmentType) {
                LOG_INFO("Wrong response handled! Got type " << params.managmentType << " instead of " << data[fsmId].pendingConfirm.managmentType);
                //DSME_ASSERT(false);
                return FSM_HANDLED;
            }
            if (data[fsmId].pendingConfirm.direction != params.direction) {
                LOG_INFO("Wrong response handled! Got direction " << params.direction << " instead of " << data[fsmId].pendingConfirm.direction);
                //DSME_ASSERT(false);
                return FSM_HANDLED;
            }

            params.status = event.management.status;

            this->dsme.getMLME_SAP().getDSME_GTS().notify_confirm(params);

            if (event.management.status == GTSStatus::SUCCESS) {
                if (event.management.type == ALLOCATION) {
                    if (checkAndHandleGTSDuplicateAllocation(event.replyNotifyCmd.getSABSpec(), event.deviceAddr, true)) { // TODO issue #3
                        uint8_t numSlotsOk = event.replyNotifyCmd.getSABSpec().getSubBlock().count(true);

                        if (numSlotsOk == 0) {
                            event.management.status = GTSStatus::DENIED;
                        } else {
                            DSME_ASSERT(false); /* This case is not handled properly, better use only one slot per request */
                        }
                    } else {
                        actUpdater.approvalReceived(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
                    }
                }

                /* the requesting node has to notify its one hop neighbors */
                DSMEMessage* msg_notify = dsme.getPlatform().getEmptyMessage();
                event.replyNotifyCmd.setDestinationAddress(event.deviceAddr);
                event.replyNotifyCmd.prependTo(msg_notify);
                if (!sendGTSCommand(fsmId, msg_notify, event.management, CommandFrameIdentifier::DSME_GTS_NOTIFY,
                        IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS)) {
                    // TODO should this be signaled to the upper layer?
                    LOG_DEBUG("NOTIFY could not be sent");
                    actUpdater.notifyAccessFailure(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
                    dsme.getPlatform().releaseMessage(msg_notify);
                    return transition(fsmId, &GTSManager::stateIdle);
                } else {
                    return transition(fsmId, &GTSManager::stateSending);
                }
            } else if (event.management.status == GTSStatus::NO_DATA) { // misuse NO_DATA to signal that the destination was busy
                //actUpdater.requestAccessFailure(event.requestCmd.getSABSpec(), event.management, event.deviceAddr);
                actUpdater.responseTimeout(event.requestCmd.getSABSpec(), event.management, event.deviceAddr);
                return transition(fsmId, &GTSManager::stateIdle);
            } else {
                DSME_ASSERT(event.management.status == GTSStatus::DENIED);
                actUpdater.disapproved(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);
                return transition(fsmId, &GTSManager::stateIdle);
            }
        }

        case GTSEvent::CFP_STARTED: {
            if (isTimeoutPending(fsmId)) {
                LOG_DEBUG("GTS timeout for response");
                mlme_sap::DSME_GTS_confirm_parameters &pendingConfirm = data[fsmId].pendingConfirm;

                actUpdater.responseTimeout(pendingConfirm.dsmeSABSpecification, data[fsmId].pendingManagement,
                        pendingConfirm.deviceAddress);
                pendingConfirm.status = GTSStatus::GTS_Status::NO_DATA;
                this->dsme.getMLME_SAP().getDSME_GTS().notify_confirm(pendingConfirm);
                return transition(fsmId, &GTSManager::stateIdle);
            } else {
                return FSM_HANDLED;
            }
        }
        default:
            DSME_ASSERT(false);
            return FSM_IGNORED;
    }
}

fsmReturnStatus GTSManager::stateWaitForNotify(GTSEvent& event) {
    int8_t fsmId = event.getFsmId();
    LOG_DEBUG("GTS Event handled: '" << signalToString(event.signal) << "' (" << stateToString(&GTSManager::stateWaitForNotify) << ")" << "[" << (uint16_t)fsmId << "]");

    switch (event.signal) {
        case GTSEvent::ENTRY_SIGNAL:
            data[fsmId].superframesInCurrentState = 0;
            return FSM_HANDLED;
        case GTSEvent::EXIT_SIGNAL:
            return FSM_IGNORED;

        case GTSEvent::MLME_REQUEST_ISSUED:
        case GTSEvent::MLME_RESPONSE_ISSUED:
        case GTSEvent::RESPONSE_CMD_FOR_ME:
        case GTSEvent::SEND_COMPLETE:
            DSME_ASSERT(false);
            return FSM_IGNORED;

        case GTSEvent::NOTIFY_CMD_FOR_ME: {
            // TODO! DSME_ASSERT((state == State::SENDING && cmdToSend == DSME_GTS_REPLY) || state == State::WAIT_FOR_NOTIFY); // TODO what if the notify comes too late, probably send a deallocation again???
            actUpdater.notifyReceived(event.replyNotifyCmd.getSABSpec(), event.management, event.deviceAddr);

            /* If the DSME-GTS Destination address is the same as the macShortAddress, the device shall notify the next higher
             * layer of the receipt of the DSME-GTS notify command frame using MLME-COMM- STATUS.indication */
            // TODO also for DEALLOCATION?
            mlme_sap::COMM_STATUS_indication_parameters params;
            params.pANId = event.header.getSrcPANId();
            params.srcAddrMode = event.header.getFrameControl().srcAddrMode;
            params.srcAddr = event.header.getSrcAddr();
            params.dstAddrMode = event.header.getFrameControl().dstAddrMode;
            params.dstAddr = event.header.getDestAddr(); //TODO: Header destination address of GTS destination address?
            params.status = CommStatus::SUCCESS;

            dsme.getMLME_SAP().getCOMM_STATUS().notify_indication(params);

            return transition(fsmId, &GTSManager::stateIdle);
        }

        case GTSEvent::CFP_STARTED: {
            if (isTimeoutPending(fsmId)) {
                LOG_DEBUG("GTS timeout for notify");
                actUpdater.notifyTimeout(data[fsmId].pendingConfirm.dsmeSABSpecification, data[fsmId].pendingManagement,
                        data[fsmId].pendingConfirm.deviceAddress);

                mlme_sap::COMM_STATUS_indication_parameters params;
                // TODO also fill other fields
                params.status = CommStatus::Comm_Status::TRANSACTION_EXPIRED;
                this->dsme.getMLME_SAP().getCOMM_STATUS().notify_indication(params);
                return transition(fsmId, &GTSManager::stateIdle);
            } else {
                return FSM_HANDLED;
            }
        }

        default:
            DSME_ASSERT(false);
            return FSM_IGNORED;
    }
}

/*****************************
 * Actions
 *****************************/

const char* GTSManager::signalToString(uint8_t signal) {
    switch (signal) {
        case GTSEvent::EMPTY_SIGNAL:
            return "EMPTY_SIGNAL";
        case GTSEvent::ENTRY_SIGNAL:
            return "ENTRY_SIGNAL";
        case GTSEvent::EXIT_SIGNAL:
            return "EXIT_SIGNAL";
        case GTSEvent::MLME_REQUEST_ISSUED:
            return "MLME_REQUEST_ISSUED";
        case GTSEvent::MLME_RESPONSE_ISSUED:
            return "MLME_RESPONSE_ISSUED";
        case GTSEvent::RESPONSE_CMD_FOR_ME:
            return "RESPONSE_CMD_FOR_ME";
        case GTSEvent::NOTIFY_CMD_FOR_ME:
            return "NOTIFY_CMD_FOR_ME";
        case GTSEvent::CFP_STARTED:
            return "CFP_STARTED";
        case GTSEvent::SEND_COMPLETE:
            return "SEND_COMPLETE";
        default:
            return "UNKNOWN";
    }
}

const char* GTSManager::stateToString(GTSManagerFSM_t::state_t state) {
    if(state == &GTSManager::stateBusy) {
        return "BUSY";
    } else if(state == &GTSManager::stateIdle) {
        return "IDLE";
    } else if(state == &GTSManager::stateSending) {
        return "SENDING";
    } else if(state == &GTSManager::stateWaitForResponse) {
        return "WAITFORRESPONSE";
    } else if(state == &GTSManager::stateWaitForNotify) {
        return "WAITFORNOTIFY";
    } else {
        return "UNKNOWN";
    }
}

void GTSManager::actionSendImmediateNegativeResponse(GTSEvent& event) {
    int8_t fsmId = event.getFsmId();
    DSME_ASSERT(event.signal == GTSEvent::MLME_RESPONSE_ISSUED);

    DSMEMessage* msg = dsme.getPlatform().getEmptyMessage();
    event.replyNotifyCmd.prependTo(msg);

    LOG_INFO(
            "Sending a negative response to a GTS-REQUEST to " << event.replyNotifyCmd.getDestinationAddress() << " due to a TRANSACTION_OVERFLOW");
    uint16_t destinationShortAddress = event.replyNotifyCmd.getDestinationAddress();
    event.management.status = GTSStatus::NO_DATA; // misuse NO_DATA to signal that the destination was busy
    if (!sendGTSCommand(fsmId, msg, event.management, CommandFrameIdentifier::DSME_GTS_REPLY, destinationShortAddress, false)) {
        LOG_DEBUG("Could not send REPLY");
        dsme.getPlatform().releaseMessage(msg);
    }
}


void GTSManager::actionReportBusyNotify(GTSEvent& event) {
    //LOG_DEBUG("BusyNotify on event '" << signalToString(event.signal) << "' (" << stateToString(this->getState()) << ")");

    mlme_sap::DSME_GTS_confirm_parameters busyConfirm;
    busyConfirm.deviceAddress = event.deviceAddr;
    busyConfirm.managmentType = event.management.type;
    busyConfirm.direction = event.management.direction;
    busyConfirm.prioritizedChannelAccess = event.management.prioritizedChannelAccess;
    busyConfirm.dsmeSABSpecification = event.requestCmd.getSABSpec();
    busyConfirm.status = GTSStatus::TRANSACTION_OVERFLOW;
    this->dsme.getMLME_SAP().getDSME_GTS().notify_confirm(busyConfirm);
}

void GTSManager::actionReportBusyCommStatus(GTSEvent& event) {
    //LOG_DEBUG("BusyCommstatus on event '" << signalToString(event.signal) << "' (" << stateToString(this->getState()) << ")");

    mlme_sap::COMM_STATUS_indication_parameters params;
    // TODO also fill other fields
    params.status = CommStatus::Comm_Status::TRANSACTION_OVERFLOW;
    this->dsme.getMLME_SAP().getCOMM_STATUS().notify_indication(params);
    return;
}

/*****************************
 * External interfaces
 *****************************/

bool GTSManager::handleMLMERequest(uint16_t deviceAddr, GTSManagement &man, GTSRequestCmd &cmd) {
    int8_t fsmId = getFsmIdForRequest();
    return dispatch(fsmId, GTSEvent::MLME_REQUEST_ISSUED, deviceAddr, man, cmd);
}

bool GTSManager::handleMLMEResponse(GTSManagement man, GTSReplyNotifyCmd reply) {
    uint16_t destinationAddress = reply.getDestinationAddress();
    int8_t fsmId = getFsmIdForResponse(destinationAddress);
    return dispatch(fsmId, GTSEvent::MLME_RESPONSE_ISSUED, destinationAddress, man, reply);
}

bool GTSManager::handleGTSRequest(DSMEMessage *msg) {
    // This can be directly passed to the upper layer.
    // There is no need to go over the state machine!
    uint16_t sourceAddr = msg->getHeader().getSrcAddr().getShortAddress();
    GTSManagement man;
    man.decapsulateFrom(msg);
    GTSRequestCmd req;
    req.decapsulateFrom(msg);

    mlme_sap::DSME_GTS_indication_parameters params;
    params.deviceAddress = sourceAddr;
    params.managmentType = man.type;
    params.direction = man.direction;
    params.prioritizedChannelAccess = man.prioritizedChannelAccess;
    params.numSlot = req.getNumSlots();
    params.preferredSuperframeID = req.getPreferredSuperframeID();
    params.preferredSlotID = req.getPreferredSlotID();
    params.dsmeSABSpecification = req.getSABSpec();

    if(man.type == ManagementType::DUPLICATED_ALLOCATION_NOTIFICATION) {
        dsme.getMAC_PIB().macDSMESAB.addOccupiedSlots(req.getSABSpec());
        actUpdater.duplicateAllocation(req.getSABSpec());
    }

    this->dsme.getMLME_SAP().getDSME_GTS().notify_indication(params);
    return true;
}

bool GTSManager::handleGTSResponse(DSMEMessage *msg) {
    GTSManagement management;
    GTSReplyNotifyCmd replyNotifyCmd;
    management.decapsulateFrom(msg);
    replyNotifyCmd.decapsulateFrom(msg);

    if (replyNotifyCmd.getDestinationAddress() == dsme.getMAC_PIB().macShortAddress) {
        int8_t fsmId = getFsmIdFromResponseForMe(msg);
        data[fsmId].responsePartnerAddress = IEEE802154MacAddress::NO_SHORT_ADDRESS;
        return dispatch(fsmId, GTSEvent::RESPONSE_CMD_FOR_ME, msg, management, replyNotifyCmd);
    }
    else if(management.status == GTSStatus::SUCCESS) {
        // Response overheared -> Add to the SAB regardless of the current state
        if (management.type == ManagementType::ALLOCATION) {
            if (!checkAndHandleGTSDuplicateAllocation(replyNotifyCmd.getSABSpec(), msg->getHeader().getSrcAddr().getShortAddress(), false)) {
                // If there is no conflict, the device shall update macDSMESAB according to the DSMESABSpecification in this
                // command frame to reflect the neighbor's newly allocated DSME-GTSs
                this->dsme.getMAC_PIB().macDSMESAB.addOccupiedSlots(replyNotifyCmd.getSABSpec());
            }
        } else if (management.type == ManagementType::DEALLOCATION) {
            this->dsme.getMAC_PIB().macDSMESAB.removeOccupiedSlots(replyNotifyCmd.getSABSpec());
        }
    }
    else {
        // A denied request should not be sent via broadcast!
        DSME_ASSERT(false);
    }

    return true;
}

bool GTSManager::handleGTSNotify(DSMEMessage* msg) {
    GTSManagement management;

    management.decapsulateFrom(msg);

    if(management.type != ManagementType::ALLOCATION && management.type != ManagementType::DEALLOCATION) {
        return true;
    }

    GTSReplyNotifyCmd replyNotifyCmd;
    replyNotifyCmd.decapsulateFrom(msg);

    if (replyNotifyCmd.getDestinationAddress() == dsme.getMAC_PIB().macShortAddress) {
        int8_t fsmId = getFsmIdFromNotifyForMe(msg);
        data[fsmId].notifyPartnerAddress = IEEE802154MacAddress::NO_SHORT_ADDRESS;
        return dispatch(fsmId, GTSEvent::NOTIFY_CMD_FOR_ME, msg, management, replyNotifyCmd);
    } else {
        // Notify overheared -> Add to the SAB regardless of the current state
        if (management.type == ManagementType::ALLOCATION) {
            if (!checkAndHandleGTSDuplicateAllocation(replyNotifyCmd.getSABSpec(), msg->getHeader().getSrcAddr().getShortAddress(), false)) {
                // If there is no conflict, the device shall update macDSMESAB according to the DSMESABSpecification in this
                // command frame to reflect the neighbor's newly allocated DSME-GTSs
                this->dsme.getMAC_PIB().macDSMESAB.addOccupiedSlots(replyNotifyCmd.getSABSpec());
            }
        } else if (management.type == ManagementType::DEALLOCATION) {
            this->dsme.getMAC_PIB().macDSMESAB.removeOccupiedSlots(replyNotifyCmd.getSABSpec());
        }
    }
    return true;
}

bool GTSManager::handleSlotEvent(uint8_t slot, uint8_t superframe) {
    if (slot == dsme.getMAC_PIB().helper.getFinalCAPSlot() + 1) {
        for (uint8_t i = 0; i < GTS_STATE_MULTIPLICITY; ++i) {
            data[i].superframesInCurrentState++;
        }

        // also execute this during non-idle phases
        if (superframe == 0) {
            for (DSMEAllocationCounterTable::iterator it = dsme.getMAC_PIB().macDSMEACT.begin(); it != dsme.getMAC_PIB().macDSMEACT.end();
                    it++) {
                // New multi-superframe started, so increment the idle counter according to 5.1.10.5.3
                // TODO for TX this should be the link quality counter, but this is redundant
                it->incrementIdleCounter(); // gets reset to zero on RX or TX
            }
        }

        for (uint8_t i = 0; i < GTS_STATE_MULTIPLICITY; ++i) {
            if (getState(i) == &GTSManager::stateWaitForNotify || getState(i) == &GTSManager::stateWaitForResponse) {
                dispatch(i, GTSEvent::CFP_STARTED);
            }
        }

        int8_t fsmId = getFsmIdIdle();
        if (fsmId >= 0) {
            return dispatch(fsmId, GTSEvent::CFP_STARTED);
        } else {
            return true;
        }
    } else {
        return true;
    }
}

bool GTSManager::onCSMASent(DSMEMessage* msg, CommandFrameIdentifier cmdId, DataStatus::Data_Status status, uint8_t numBackoffs) {
    GTSManagement management;
    management.decapsulateFrom(msg);

    bool returnStatus;
    if (management.type == ManagementType::DUPLICATED_ALLOCATION_NOTIFICATION) {
        // Sending the duplicate allocation is not handled by the state machine, since
        // it is just a state-less notification (at least for our interpretation of the standard)
        LOG_DEBUG("DUPLICATED_ALLOCATION_NOTIFICATION sent");
        returnStatus = true;
    } else {
        int8_t validFsmId = -1;

        for(uint8_t i = 0; i < GTS_STATE_MULTIPLICITY; ++i) {
            if(getState(i) == &GTSManager::stateSending && data[i].msgToSend == msg) {
                validFsmId = i;
                break;
            }
        }

        if (validFsmId >= 0 && validFsmId < GTS_STATE_MULTIPLICITY) {
            if (status != DataStatus::SUCCESS) {
                LOG_DEBUG("GTSManager::onCSMASent transmission failure: " << status);
            }
            returnStatus = dispatch(validFsmId, GTSEvent::SEND_COMPLETE, msg, management, cmdId, status);
        } else {
            // If the ACK was lost, but the message itself was delivered successfully,
            // the RESPONSE or NOTIFY might already have been handled properly.
            // Same holds if the current state is not sending (see there)
            // TODO What about the states of the receiver and the neighbours?
            LOG_DEBUG("Outdated message");
            returnStatus = true;
        }
    }

    dsme.getPlatform().releaseMessage(msg);
    return returnStatus;
}


/*****************************
 * Internal helpers
 *****************************/

bool GTSManager::checkAndHandleGTSDuplicateAllocation(DSMESABSpecification& sabSpec, uint16_t addr, bool allChannels) {
    DSMEAllocationCounterTable &macDSMEACT = this->dsme.getMAC_PIB().macDSMEACT;

    bool duplicateFound = false;

    GTSRequestCmd dupReq;
    dupReq.getSABSpec().setSubBlockLengthBytes(sabSpec.getSubBlockLengthBytes()); // = new DSME_GTSRequestCmd("gts-request-duplication");
    dupReq.getSABSpec().setSubBlockIndex(sabSpec.getSubBlockIndex());

    for (DSMESABSpecification::SABSubBlock::iterator it = sabSpec.getSubBlock().beginSetBits(); it != sabSpec.getSubBlock().endSetBits();
            it++) {

        DSMEAllocationCounterTable::iterator actElement = macDSMEACT.find(sabSpec.getSubBlockIndex(), (*it) / dsme.getMAC_PIB().helper.getNumChannels());
        if (actElement != macDSMEACT.end() && (allChannels || actElement->getChannel() == (*it) % dsme.getMAC_PIB().helper.getNumChannels())) {
            LOG_INFO("Duplicate allocation " << (uint16_t)(actElement->getGTSlotID()+9) << " " << (uint16_t)sabSpec.getSubBlockIndex()
                    << " " << (uint16_t)actElement->getChannel());

            duplicateFound = true;
            dupReq.getSABSpec().getSubBlock().set(*it, true);

            // clear bit so the sabSpec can be used in a notification
            sabSpec.getSubBlock().set(*it, false);
        }
    }

    if (duplicateFound) {
        LOG_INFO("Duplicate allocation detected, informing originating device.");
        DSMEMessage* msg = dsme.getPlatform().getEmptyMessage();
        dupReq.prependTo(msg);
        GTSManagement man;
        man.type = ManagementType::DUPLICATED_ALLOCATION_NOTIFICATION;
        man.status = GTSStatus::SUCCESS;

        // this request expects no reply, so do not use usual request command
        // also do not handle this via the state machine
        if(!sendGTSCommand(-1, msg, man, CommandFrameIdentifier::DSME_GTS_REQUEST, addr)) {
            // TODO should this be signaled to the upper layer?
            LOG_DEBUG("Could not send DUPLICATED_ALLOCATION_NOTIFICATION");
            dsme.getPlatform().releaseMessage(msg);
        }
    }

    return duplicateFound;
}

bool GTSManager::isTimeoutPending(uint8_t fsmId) {
    // According to the IEEE 802.15.4e standard, the macMaxFrameTotalWaitTime should be used for the timeout.
    // This is not enough, for example due to queuing of the reply and not considering the GTS times.
    // It was changed in the IEEE 802.15.4-2015 standard to macResponseWaitTime (see e.g. Figure 6-57).
    // macResponseWaitTime is given in aBaseSuperframeDurations (that do not include the superframe order)
    LOG_DEBUG("superframesInCurrentState: " << data[fsmId].superframesInCurrentState << "("
            << data[fsmId].superframesInCurrentState*(1 << dsme.getMAC_PIB().macSuperframeOrder) << "/"
            << dsme.getMAC_PIB().macResponseWaitTime
            << ")");

    return (data[fsmId].superframesInCurrentState*(1 << dsme.getMAC_PIB().macSuperframeOrder) > dsme.getMAC_PIB().macResponseWaitTime);
}

bool GTSManager::sendGTSCommand(uint8_t fsmId, DSMEMessage* msg, GTSManagement& man, CommandFrameIdentifier commandId, uint16_t dst, bool reportOnSent) {
    man.prependTo(msg);

    MACCommand cmd;
    cmd.setCmdId(commandId);
    cmd.prependTo(msg);

    msg->getHeader().setDstAddr(dst);
    msg->getHeader().setSrcAddrMode(AddrMode::SHORT_ADDRESS);
    msg->getHeader().setSrcAddr(dsme.getMAC_PIB().macShortAddress);
    msg->getHeader().setDstAddrMode(AddrMode::SHORT_ADDRESS);
    msg->getHeader().setAckRequest(true);
    msg->getHeader().setFrameType(IEEE802154eMACHeader::FrameType::COMMAND);

    // The DUPLICATED_ALLOCATION_NOTIFICATION will be sent regardless of the current state and expects no response.
    // For example a DISALLOW REPSPONE will only be sent during BUSY, but the fact that it do not expect a notify
    // is handled inside of the onCSMASent.
    if(reportOnSent && (man.type != ManagementType::DUPLICATED_ALLOCATION_NOTIFICATION)) {
        DSME_ASSERT(fsmId >= 0 && fsmId < GTS_STATE_MULTIPLICITY);
        data[fsmId].cmdToSend = commandId;
        data[fsmId].msgToSend = msg;
    }

    return dsme.getMessageDispatcher().sendInCAP(msg);
}

void GTSManager::preparePendingConfirm(GTSEvent& event) {
    int8_t fsmId = event.getFsmId();

    data[fsmId].pendingManagement = event.management;
    data[fsmId].pendingConfirm.deviceAddress = event.deviceAddr;
    data[fsmId].pendingConfirm.managmentType = event.management.type;
    data[fsmId].pendingConfirm.direction = event.management.direction;
    data[fsmId].pendingConfirm.prioritizedChannelAccess = event.management.prioritizedChannelAccess;
    if(event.signal == GTSEvent::MLME_REQUEST_ISSUED) {
        data[fsmId].pendingConfirm.dsmeSABSpecification = event.requestCmd.getSABSpec();
    }
    else if(event.signal == GTSEvent::MLME_RESPONSE_ISSUED) {
        data[fsmId].pendingConfirm.dsmeSABSpecification = event.replyNotifyCmd.getSABSpec();
    }
    else {
        DSME_ASSERT(false);
    }
}

/*****************************
 * FSM identification helpers
 *****************************/

int8_t GTSManager::getFsmIdIdle() {
    for(uint8_t i = 0; i < GTS_STATE_MULTIPLICITY; ++i) {
        if(getState(i) == &GTSManager::stateIdle) {
            return i;
        }
    }
    return -1;
}

int8_t GTSManager::getFsmIdForRequest() {
    int8_t fsmId = getFsmIdIdle();
    if(fsmId < 0) {
        return GTS_STATE_MULTIPLICITY;
    } else {
        return fsmId;
    }
}

int8_t GTSManager::getFsmIdForResponse(uint16_t destinationAddress) {
    int8_t fsmId = getFsmIdIdle();
    if(fsmId < 0) {
        return GTS_STATE_MULTIPLICITY;
    } else {
        return fsmId;
    }
}

int8_t GTSManager::getFsmIdFromResponseForMe(DSMEMessage* msg) {
    uint16_t srcAddress = msg->getHeader().getSrcAddr().getShortAddress();
    for(uint8_t i = 0; i < GTS_STATE_MULTIPLICITY; ++i) {
        if (getState(i) == &GTSManager::stateWaitForResponse && data[i].responsePartnerAddress == srcAddress) {
            return i;
        }
    }
    return GTS_STATE_MULTIPLICITY;
}

int8_t GTSManager::getFsmIdFromNotifyForMe(DSMEMessage* msg) {
    uint16_t srcAddress = msg->getHeader().getSrcAddr().getShortAddress();
    for(uint8_t i = 0; i < GTS_STATE_MULTIPLICITY; ++i) {
        if (getState(i) == &GTSManager::stateWaitForNotify && data[i].notifyPartnerAddress == srcAddress) {
            return i;
        }
    }
    return GTS_STATE_MULTIPLICITY;
}


} /* dsme */
